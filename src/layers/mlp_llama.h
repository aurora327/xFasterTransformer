// Copyright (c) 2023 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// ============================================================================
#pragma once
#include "bert_util.h"
#include "copy_util.h"
#include "debugger.h"
#include "decoder_util.h"
#include "matmul_helper.h"
#include "rmsnorm_kernels.h"
#include "simple_mem_pool.h"
#include "singleton.h"
#include "timeline.h"

// C++ implementation for the python code in modeling_llama.py:
// residual = hidden_states
// hidden_states = self.post_attention_layernorm(hidden_states)
// hidden_states = self.mlp(hidden_states)
// hidden_states = residual + hidden_states
//
// While LlamaMLP is like:
// def forward(self, x):
//         return self.down_proj(self.act_fn(self.gate_proj(x)) * self.up_proj(x))
// But please also be noted: we extended the MLP to include layer norm
template <typename WeiT, typename InT = float, typename ImT = float, typename OutT = float>
class LlamaMLP : public SingletonBase<LlamaMLP<WeiT>> {
public:
    LlamaMLP() {}

    LlamaMLP(DecoderContext *ctx) {}

    // OriWeiT: float or int8_t
    template <typename OriWeiT>
    void setWeights(DecoderContext *ctx, const OriWeiT *gateW, const float *gateS, const float *gateZ,
            const float * /*unused*/, const OriWeiT *upW, const float *upS, const float *upZ, const float * /*unused*/,
            const float *normW, const float * /*unused*/, const OriWeiT *downW, const float *downS, const float *downZ,
            bool trans = true) {
        int hiddenSize = ctx->hiddenSize;
        int imSize = ctx->intermediateSize;

        REQUIRES(ctx->actType == DecoderContext::SILU, "unsupported activation.");

        // Vertically split the gate weight and up weight
        hpj::Matrix<WeiT> quantizedGateWeight, quantizedUpWeight, quantizedDownWeight;

        auto it = SplitUtil::getTaskRange(imSize, ctx->numSplit, ctx->splitIdx);
        downWeight.Resize(it.second - it.first, hiddenSize);

        ctx->mmHelper->convertWeight(ctx, trans, hiddenSize, imSize, gateW, gateS, gateZ, true, quantizedGateWeight,
                gateWeightScale, gateWeightZero, gateWeightSum);
        ctx->mmHelper->convertWeight(ctx, trans, hiddenSize, imSize, upW, upS, upZ, true, quantizedUpWeight,
                upWeightScale, upWeightZero, upWeightSum);

        if (!enableCATMLP()) {
            gateWeight.Resize(hiddenSize, it.second - it.first);
            upWeight.Resize(hiddenSize, it.second - it.first);
            ctx->mmHelper->packWeight(trans, quantizedGateWeight, gateWeight);
            ctx->mmHelper->packWeight(trans, quantizedUpWeight, upWeight);
        } else {
            hpj::Matrix<WeiT> quantizedCatWeights;
            catGateUpWeights(quantizedGateWeight, quantizedUpWeight, gateWeightScale, gateWeightZero, gateWeightSum,
                    upWeightScale, upWeightZero, upWeightSum, quantizedCatWeights, catWeightsScale, catWeightsZero,
                    catWeightsSum);
            quantizedGateWeight.Release();
            quantizedUpWeight.Release();
            catWeights.Resize(quantizedCatWeights.Rows(), quantizedCatWeights.Cols());
            ctx->mmHelper->packWeight(trans, quantizedCatWeights, catWeights);
        }
        // Horizontally split the down weight
        ctx->mmHelper->convertWeight(ctx, trans, imSize, hiddenSize, downW, downS, downZ, false,
                quantizedDownWeight, downWeightScale, downWeightZero, downWeightSum);
        ctx->mmHelper->packWeight(trans, quantizedDownWeight, downWeight);

#ifdef DEBUG
        dbg.debugPrint("quantizedGateWeight:\n");
        dbg.dumpMatrix(quantizedGateWeight);

        dbg.debugPrint("quantizedUpWeight:\n");
        dbg.dumpMatrix(quantizedUpWeight);

        dbg.debugPrint("quantizedDownWeight:\n");
        dbg.dumpMatrix(quantizedDownWeight);
#endif

        // LlamaRMSNorm
        if (normW) {
            normWeight.Resize(hiddenSize);
            memcpy(normWeight.Data(), normW, sizeof(float) * hiddenSize);
        }
    }

#ifdef DEBUG
    void setDebugger(const Debugger &debugger) { this->dbg = debugger; }
#endif

    // Forward for FFN (Feed Forward Network)
    void forward(DecoderContext *ctx, InT *input, OutT *output, int iStride, int oStride,
            bool doLnBefore = true /*not used*/) {
        TimeLine t("LlamaMLP");
        const int M = ctx->batchSize * ctx->inputSeqLen;
        const int hiddenSize = ctx->hiddenSize;

        static_assert(sizeof(ctx->normBuf.Data()[0]) >= sizeof(ImT), "normBuff is not big enough!");

        hpj::Matrix<InT> inBuffer(input, M, hiddenSize, iStride);
        hpj::Matrix<OutT> outBuffer(output, M, hiddenSize, oStride);
        hpj::Matrix<ImT> normBuffer(
                (ImT *)ctx->normBuf.Data(), ctx->normBuf.Rows(), ctx->normBuf.Cols(), ctx->normBuf.Stride());

        if (doLnBefore == true) {
            xft::rmsNorm(normBuffer.Data(), inBuffer.Data(), normWeight.Data(), M, hiddenSize, inBuffer.Stride(),
                    normBuffer.Stride(), 1e-6);
        }

#ifdef DEBUG
        dbg.debugPrint("LayerNorm before MLP:\n");
        dbg.dumpMatrix(normBuffer);
#endif

        if (!enableCATMLP()) {
            hpj::Matrix<ImT> imBuffer(
                    (ImT *)ctx->imOut.Data(), ctx->imOut.Rows(), ctx->imOut.Cols(), ctx->imOut.Stride());
            gateProj(ctx, doLnBefore ? normBuffer : inBuffer, imBuffer);

#ifdef DEBUG
            dbg.debugPrint("gateWeight:\n");
            dbg.dumpMatrix(gateWeight);
            dbg.debugPrint("gate output:\n");
            dbg.dumpMatrix(imBuffer);
#endif

            upProj(ctx, doLnBefore ? normBuffer : inBuffer, imBuffer);

#ifdef DEBUG
            dbg.debugPrint("upWeight:\n");
            dbg.dumpMatrix(upWeight);
            dbg.debugPrint("up output:\n");
            dbg.dumpMatrix(imBuffer);
#endif
            downProj(ctx, imBuffer, outBuffer, inBuffer, ctx->splitIdx == 0);

        } else {
            int M = normBuffer.Rows();
            int N = catWeights.Cols();
            hpj::Matrix<ImT> imBuffer((ImT *)ctx->imOut.Data(), M, N, N);

            // Need to allocate extra buffer as oneDNN does not support the case of stride > cols
            const int cols = N / 2;
            auto bufSize = M * cols * sizeof(ImT);
            ImT *t = (ImT *)SimpleMemPool::instance().getBuffer("mlp_silu", bufSize);
            hpj::Matrix<ImT> siluBuf(t, M, cols, cols);

            catGateUpProj(ctx, doLnBefore ? normBuffer : inBuffer, imBuffer, siluBuf);
#ifdef DEBUG
            dbg.debugPrint("catWeights:\n");
            dbg.dumpMatrix(catWeights);
            dbg.debugPrint("gateUp output:\n");
            dbg.dumpMatrix(siluBuf);
#endif
            downProj(ctx, siluBuf, outBuffer, inBuffer, ctx->splitIdx == 0);
        }

#ifdef DEBUG
        dbg.debugPrint("downWeight:\n");
        dbg.dumpMatrix(downWeight);
        dbg.debugPrint("residential:\n");
        dbg.dumpMatrix(inBuffer);
        dbg.debugPrint("final output:\n");
        dbg.dumpMatrix(outBuffer);
#endif
    }

private:
    void gateProj(DecoderContext *ctx, hpj::Matrix<InT> &input, hpj::Matrix<ImT> &output) {
        TimeLine t("GateProj");

        assert(input.Rows() == output.Rows());
        assert(input.Cols() == gateWeight.Rows());
        assert(gateWeight.Cols() == output.Cols());

        int M = input.Rows(), N = output.Cols(), K = input.Cols();
        int lda = input.Stride(), ldc = output.Stride();

        const InT *A = input.Data();
        const WeiT *B = gateWeight.Data();
        const float *scaleB = gateWeightScale.Data();
        const float *zeroB = gateWeightZero.Data();
        const float *sumB = gateWeightSum.Data();
        ImT *C = output.Data();

        ctx->mmHelper->compute_silu(false, M, N, K, 1.0f, A, lda, B, scaleB, zeroB, sumB, 0.0f, C, ldc);
    }

    void upProj(DecoderContext *ctx, hpj::Matrix<InT> &input, hpj::Matrix<ImT> &output) {
        TimeLine t("UpProj");

        assert(input.Rows() == output.Rows());
        assert(input.Cols() == upWeight.Rows());
        assert(upWeight.Cols() == output.Cols());

        int M = input.Rows(), N = output.Cols(), K = input.Cols();
        int lda = input.Stride(), ldc = output.Stride();

        const InT *A = input.Data();
        const WeiT *B = upWeight.Data();
        const float *scaleB = upWeightScale.Data();
        const float *zeroB = upWeightZero.Data();
        const float *sumB = upWeightSum.Data();
        ImT *C = output.Data();

        ctx->mmHelper->compute_resmul(false, M, N, K, 1.0f, A, lda, B, scaleB, zeroB, sumB, 0.0f, C, ldc, C, ldc);
    }

    void downProj(DecoderContext *ctx, hpj::Matrix<ImT> &input, hpj::Matrix<OutT> &output,
            hpj::Matrix<InT> &residential, bool isMaster) {
        TimeLine t("DownProj");

        assert(input.Rows() == output.Rows());
        if (!enableCATMLP())
            assert(input.Cols() == downWeight.Rows());
        else
            assert(input.Cols() == 2 * downWeight.Rows());
        assert(downWeight.Cols() == output.Cols());

        int M = input.Rows(), N = output.Cols(), K = downWeight.Rows();
        int lda = input.Stride(), ldc = output.Stride(), ldr = residential.Stride();

        const ImT *A = input.Data();
        const WeiT *B = downWeight.Data();
        const float *scaleB = downWeightScale.Data();
        const float *zeroB = downWeightZero.Data();
        const float *sumB = downWeightSum.Data();
        OutT *C = output.Data();
        const InT *R = residential.Data();

        if (isMaster) {
            ctx->mmHelper->compute_residential(
                    false, M, N, K, 1.0f, A, lda, B, scaleB, zeroB, sumB, 0.0f, C, ldc, NULL, R, ldr);
        } else {
            ctx->mmHelper->compute(false, M, N, K, 1.0f, A, lda, B, scaleB, zeroB, sumB, 0.0f, C, ldc);
        }
    }

    template <typename T1, typename T2>
    void catGateUpProj(DecoderContext *ctx, hpj::Matrix<T1> &input, hpj::Matrix<T2> &output, hpj::Matrix<T2> &siluBuf) {
        TimeLine t("catGateUpProj");

        assert(input.Rows() == output.Rows());
        assert(input.Cols() == catWeights.Rows());
        assert(catWeights.Cols() == output.Cols());

        int M = input.Rows(), N = output.Cols(), K = input.Cols();
        int lda = input.Stride(), ldc = output.Stride();

        const T1 *A = input.Data();
        const WeiT *B = catWeights.Data();
        const float *scaleB = catWeightsScale.Data();
        const float *zeroB = catWeightsZero.Data();
        const float *sumB = catWeightsSum.Data();
        T2 *C = output.Data();

        ctx->mmHelper->compute(false, M, N, K, 1.0f, A, lda, B, scaleB, zeroB, sumB, 0.0f, C, ldc);

        // Compute silu on the left half and then add it with the right half
        DecoderUtil::siluSum(output, siluBuf);
    }

    void catGateUpWeights(hpj::Matrix<WeiT> &gateWeight, hpj::Matrix<WeiT> &upWeight,
            hpj::Vector<float> &gateWeightScale, hpj::Vector<float> &gateWeightZero, hpj::Vector<float> &gateWeightSum,
            hpj::Vector<float> &upWeightScale, hpj::Vector<float> &upWeightZero, hpj::Vector<float> &upWeightSum,
            hpj::Matrix<WeiT> &catWeights, hpj::Vector<float> &catWeightsScale, hpj::Vector<float> &catWeightsZero,
            hpj::Vector<float> &catWeightsSum) {
        catWeights.Resize(gateWeight.Rows(), gateWeight.Cols() + upWeight.Cols());
        catWeightsScale.Resize(gateWeightScale.Size() + upWeightScale.Size());
        catWeightsZero.Resize(gateWeightZero.Size() + upWeightZero.Size());
        catWeightsSum.Resize(gateWeightSum.Size() + upWeightSum.Size());

        int M = catWeights.Rows();
        int Stride = catWeights.Cols();
        int N = gateWeight.Cols();
        if (std::is_same_v<WeiT, uint4x2_t> || std::is_same_v<WeiT, nf4x2_t>) {
            // two values are packed into one byte
            Stride /= 2;
            N /= 2;
        }
#pragma omp parallel for
        for (uint64_t i = 0; i < M; ++i) {
            memcpy(catWeights.Data() + i * Stride, gateWeight.Data() + i * N, N * sizeof(WeiT));
            memcpy(catWeights.Data() + i * Stride + N, upWeight.Data() + i * N, N * sizeof(WeiT));
        }

        M = gateWeightScale.Size();
        N = upWeightScale.Size();
        memcpy(catWeightsScale.Data(), gateWeightScale.Data(), M * sizeof(float));
        memcpy(catWeightsScale.Data() + M, upWeightScale.Data(), N * sizeof(float));
        memcpy(catWeightsZero.Data(), gateWeightZero.Data(), M * sizeof(float));
        memcpy(catWeightsZero.Data() + M, upWeightZero.Data(), N * sizeof(float));
        M = gateWeightSum.Size();
        N = upWeightSum.Size();
        memcpy(catWeightsSum.Data(), gateWeightSum.Data(), M * sizeof(float));
        memcpy(catWeightsSum.Data() + M, upWeightSum.Data(), N * sizeof(float));
    }

protected:
    hpj::Matrix<WeiT> gateWeight;
    hpj::Vector<float> gateWeightScale; // For int8_t weight
    hpj::Vector<float> gateWeightZero; // For int8_t weight
    hpj::Vector<float> gateWeightSum; // For int8_t weight
    hpj::Matrix<WeiT> upWeight;
    hpj::Vector<float> upWeightScale; // For int8_t weight
    hpj::Vector<float> upWeightZero; // For int8_t weight
    hpj::Vector<float> upWeightSum; // For int8_t weight
    hpj::Matrix<WeiT> catWeights;
    hpj::Vector<float> catWeightsScale; // For int8_t weight
    hpj::Vector<float> catWeightsZero; // For int8_t weight
    hpj::Vector<float> catWeightsSum; // For int8_t weight
    hpj::Matrix<WeiT> downWeight;
    hpj::Vector<float> downWeightScale; // For int8_t weight
    hpj::Vector<float> downWeightZero; // For int8_t weight
    hpj::Vector<float> downWeightSum; // For int8_t weight

    // LlamaRMSNorm param
    hpj::Vector<float> normWeight;

#ifdef DEBUG
    Debugger dbg;
#endif
};
