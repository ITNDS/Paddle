/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "paddle/fluid/operators/batch_norm_op.h"
#include "paddle/fluid/platform/mkldnn_reuse.h"

namespace phi {
class DenseTensor;
}  // namespace phi

namespace paddle {
namespace operators {

using dnnl::memory;
using dnnl::primitive;
using dnnl::reorder;
using dnnl::stream;
using paddle::platform::MKLDNNDeviceContext;
using platform::to_void_cast;

template <typename T>
class BatchNormMKLDNNHandler : public platform::MKLDNNHandlerNoCachingT<
                                   T,
                                   dnnl::batch_normalization_forward,
                                   dnnl::batch_normalization_backward> {
 public:
  BatchNormMKLDNNHandler(const paddle::framework::ExecutionContext &ctx,
                         const dnnl::engine mkldnn_engine,
                         const Tensor *x,
                         const bool global_stats,
                         const bool test_mode)
      : platform::MKLDNNHandlerNoCachingT<T,
                                          dnnl::batch_normalization_forward,
                                          dnnl::batch_normalization_backward>(
            mkldnn_engine, ctx.GetPlace()) {
    const float epsilon = ctx.Attr<float>("epsilon");
    const bool fuse_with_relu = ctx.HasAttr("fuse_with_relu")
                                    ? ctx.Attr<bool>("fuse_with_relu")
                                    : false;

    std::vector<std::string> DataLayout_error_msg = {
        "kNHWC", "kNCHW", "kAnyLayout", "kMKLDNN"};

    // Flags are added by bitwise OR operation
    auto flags = dnnl::normalization_flags::use_scale_shift;  // 001
    if (global_stats)
      flags |= dnnl::normalization_flags::use_global_stats;  // 010
    if (fuse_with_relu && test_mode)
      flags |= dnnl::normalization_flags::fuse_norm_relu;  // 100

    this->AcquireForwardPrimitiveDescriptor(
        global_stats == true ? dnnl::prop_kind::forward_scoring
                             : dnnl::prop_kind::forward_training,
        x->mem_desc(),
        epsilon,
        flags);
  }

  BatchNormMKLDNNHandler(const paddle::framework::ExecutionContext &ctx,
                         const dnnl::engine mkldnn_engine,
                         const Tensor *in_x,
                         const Tensor *scale,
                         const Tensor *out_grad)
      : platform::MKLDNNHandlerNoCachingT<T,
                                          dnnl::batch_normalization_forward,
                                          dnnl::batch_normalization_backward>(
            mkldnn_engine, ctx.GetPlace()) {
    auto scale_tz = phi::vectorize<int64_t>(scale->dims());
    PADDLE_ENFORCE_EQ(
        scale_tz.size(),
        1,
        platform::errors::InvalidArgument(
            "Dims of scale tensor must be 1, but received scale's size is %d",
            scale_tz.size()));

    const float epsilon = ctx.Attr<float>("epsilon");

    this->AcquireForwardPrimitiveDescriptor(
        dnnl::prop_kind::forward_training,
        in_x->mem_desc(),
        epsilon,
        dnnl::normalization_flags::use_scale_shift);
    this->AcquireBackwardPrimitiveDescriptor(
        dnnl::prop_kind::backward,
        out_grad->mem_desc(),
        in_x->mem_desc(),
        epsilon,
        dnnl::normalization_flags::use_scale_shift);
  }

  std::shared_ptr<dnnl::memory> AcquireScaleShiftMemory(const Tensor *scale,
                                                        const Tensor *shift) {
    auto scale_tz = phi::vectorize(scale->dims());
    const unsigned int C = scale_tz[0];
    PADDLE_ENFORCE_EQ(
        scale_tz.size(),
        1,
        platform::errors::InvalidArgument(
            "Dims of scale tensor must be 1, but received scale's size is %d",
            scale_tz.size()));

    auto scaleshift_memory =
        this->AcquireMemoryFromPrimitive(this->fwd_pd_->weights_desc());

    // MKLDNN requires a single piece of memory for scale and shift/bias data
    auto mem_data_handle =
        reinterpret_cast<T *>(scaleshift_memory->get_data_handle());
    std::copy(scale->data<T>(), scale->data<T>() + C, mem_data_handle);
    std::copy(shift->data<T>(), shift->data<T>() + C, mem_data_handle + C);
    return scaleshift_memory;
  }

  std::shared_ptr<dnnl::memory> AcquireDiffScaleShiftMemory(
      T *diff_scaleshift_data) {
    return this->AcquireMemoryFromPrimitive(this->bwd_pd_->diff_weights_desc(),
                                            diff_scaleshift_data);
  }

  std::shared_ptr<dnnl::memory> AcquireMeanMemory(
      const phi::DenseTensor *mean) {
    const T *mean_data = mean->data<T>();
    return this->AcquireMemoryFromPrimitive(this->fwd_pd_->mean_desc(),
                                            to_void_cast<T>(mean_data));
  }

  std::shared_ptr<dnnl::memory> AcquireMeanMemory(phi::DenseTensor *mean) {
    T *mean_data = mean->mutable_data<T>(this->place_,
                                         this->fwd_pd_->mean_desc().get_size());
    return this->AcquireMemoryFromPrimitive(this->fwd_pd_->mean_desc(),
                                            mean_data);
  }

  std::shared_ptr<dnnl::memory> AcquireVarianceMemory(
      const phi::DenseTensor *variance) {
    const T *variance_data = variance->data<T>();
    return this->AcquireMemoryFromPrimitive(this->fwd_pd_->variance_desc(),
                                            to_void_cast<T>(variance_data));
  }

  std::shared_ptr<dnnl::memory> AcquireVarianceMemory(
      phi::DenseTensor *variance) {
    T *variance_data = variance->mutable_data<T>(
        this->place_, this->fwd_pd_->variance_desc().get_size());
    return this->AcquireMemoryFromPrimitive(this->fwd_pd_->variance_desc(),
                                            variance_data);
  }
};

template <typename T>
class BatchNormMKLDNNOpKernel : public paddle::framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext &ctx) const override {
    auto &dev_ctx = ctx.template device_context<MKLDNNDeviceContext>();
    const auto &mkldnn_engine = dev_ctx.GetEngine();

    const bool is_test = ctx.Attr<bool>("is_test");
    const bool use_global_stats = ctx.Attr<bool>("use_global_stats");
    const bool trainable_stats = ctx.Attr<bool>("trainable_statistics");
    const bool test_mode = is_test && (!trainable_stats);
    const bool global_stats = test_mode || use_global_stats;

    const auto *x = ctx.Input<phi::DenseTensor>("X");
    const auto *scale = ctx.Input<phi::DenseTensor>("Scale");
    const auto *shift = ctx.Input<phi::DenseTensor>("Bias");

    auto *y = ctx.Output<phi::DenseTensor>("Y");
    auto *batch_mean = ctx.Output<phi::DenseTensor>("SavedMean");
    auto *batch_variance = ctx.Output<phi::DenseTensor>("SavedVariance");
    BatchNormMKLDNNHandler<T> handler(
        ctx, mkldnn_engine, x, global_stats, test_mode);

    auto src_memory = handler.AcquireSrcMemory(x);
    auto scaleshift_memory = handler.AcquireScaleShiftMemory(scale, shift);
    auto dst_memory = handler.AcquireDstMemory(y);
    auto batch_norm_p = handler.AcquireForwardPrimitive();

    std::shared_ptr<memory> mean_memory;
    std::shared_ptr<memory> variance_memory;

    if (global_stats) {
      // mean and variance are taken from input Tensor
      const auto *mean = ctx.Input<phi::DenseTensor>("Mean");
      const auto *variance = ctx.Input<phi::DenseTensor>("Variance");

      mean_memory = handler.AcquireMeanMemory(mean);
      variance_memory = handler.AcquireVarianceMemory(variance);
    } else {
      // mean and variance are calculated and saved in output Tensor
      mean_memory = handler.AcquireMeanMemory(batch_mean);
      variance_memory = handler.AcquireVarianceMemory(batch_variance);
    }

    y->set_mem_desc(dst_memory->get_desc());

    auto &astream = platform::MKLDNNDeviceContext::tls().get_stream();
    batch_norm_p->execute(astream,
                          {{DNNL_ARG_SRC, *src_memory},
                           {DNNL_ARG_SCALE_SHIFT, *scaleshift_memory},
                           {DNNL_ARG_MEAN, *mean_memory},
                           {DNNL_ARG_VARIANCE, *variance_memory},
                           {DNNL_ARG_DST, *dst_memory}});
    astream.wait();

    if (!global_stats) {
      auto *mean_out = ctx.Output<phi::DenseTensor>("MeanOut");
      auto *variance_out = ctx.Output<phi::DenseTensor>("VarianceOut");
      const float momentum = ctx.Attr<float>("momentum");

      const unsigned int C = phi::vectorize(scale->dims())[0];

      // mkldnn only compute stats for current batch
      // so we need compute momentum stats via Eigen lib
      EigenVectorArrayMap<T> batch_mean_e(
          batch_mean->mutable_data<T>(ctx.GetPlace()), C);
      EigenVectorArrayMap<T> batch_variance_e(
          batch_variance->mutable_data<T>(ctx.GetPlace()), C);

      EigenVectorArrayMap<T> running_mean_e(
          mean_out->mutable_data<T>(ctx.GetPlace()), C);
      EigenVectorArrayMap<T> running_variance_e(
          variance_out->mutable_data<T>(ctx.GetPlace()), C);

      running_mean_e =
          running_mean_e * momentum + batch_mean_e * (1. - momentum);
      running_variance_e =
          running_variance_e * momentum + batch_variance_e * (1. - momentum);
    }
  }
};

template <typename T>
class BatchNormMKLDNNGradOpKernel : public paddle::framework::OpKernel<T> {
 public:
  void Compute(const paddle::framework::ExecutionContext &ctx) const override {
    auto &dev_ctx = ctx.template device_context<MKLDNNDeviceContext>();
    auto mkldnn_engine = dev_ctx.GetEngine();

    const auto *x = ctx.Input<phi::DenseTensor>("X");
    const auto *scale = ctx.Input<phi::DenseTensor>("Scale");
    const auto *shift = ctx.Input<phi::DenseTensor>("Bias");
    const auto *batch_mean = ctx.Input<phi::DenseTensor>("SavedMean");
    const auto *batch_variance = ctx.Input<phi::DenseTensor>("SavedVariance");
    const auto *diff_y =
        ctx.Input<phi::DenseTensor>(framework::GradVarName("Y"));
    auto *diff_x = ctx.Output<phi::DenseTensor>(framework::GradVarName("X"));
    auto *diff_scale =
        ctx.Output<phi::DenseTensor>(framework::GradVarName("Scale"));
    auto *diff_shift =
        ctx.Output<phi::DenseTensor>(framework::GradVarName("Bias"));

    BatchNormMKLDNNHandler<T> handler(ctx, mkldnn_engine, x, scale, diff_y);

    // MKLDNN requires a single piece of memory for scale and shift/bias data
    const unsigned int C = phi::vectorize(scale->dims())[0];
    const size_t scaleshift_size = 2 * C;
    std::vector<T> diff_scaleshift_data;
    diff_scaleshift_data.reserve(scaleshift_size);

    auto src_memory = handler.AcquireSrcMemory(x);
    auto mean_memory = handler.AcquireMeanMemory(batch_mean);
    auto variance_memory = handler.AcquireVarianceMemory(batch_variance);
    auto diff_dst_memory = handler.AcquireDiffDstMemory(diff_y);
    auto scaleshift_memory = handler.AcquireScaleShiftMemory(scale, shift);
    auto diff_src_memory = handler.AcquireDiffSrcMemory(diff_x);
    auto diff_scaleshift_memory =
        handler.AcquireDiffScaleShiftMemory(diff_scaleshift_data.data());
    // finally create batch_norm backward primitive
    auto batch_norm_bwd_p = handler.AcquireBackwardPrimitive();

    auto &astream = platform::MKLDNNDeviceContext::tls().get_stream();
    batch_norm_bwd_p->execute(
        astream,
        {{DNNL_ARG_SRC, *src_memory},
         {DNNL_ARG_MEAN, *mean_memory},
         {DNNL_ARG_VARIANCE, *variance_memory},
         {DNNL_ARG_DIFF_DST, *diff_dst_memory},
         {DNNL_ARG_SCALE_SHIFT, *scaleshift_memory},
         {DNNL_ARG_DIFF_SRC, *diff_src_memory},
         {DNNL_ARG_DIFF_SCALE_SHIFT, *diff_scaleshift_memory}});
    astream.wait();

    T *diff_scale_data = diff_scale->mutable_data<T>(ctx.GetPlace());
    T *diff_shift_data = diff_shift->mutable_data<T>(ctx.GetPlace());

    // copy back diff scale/shift to output tensors (diff scale/shift)
    diff_scaleshift_data.resize(scaleshift_size);
    auto it = std::begin(diff_scaleshift_data);
    std::copy(it, std::next(it, C), diff_scale_data);
    std::copy(
        std::next(it, C), std::end(diff_scaleshift_data), diff_shift_data);

    // set memory descriptor of out tensor
    diff_x->set_mem_desc(diff_src_memory->get_desc());
  }
};
}  // namespace operators
}  // namespace paddle

namespace ops = paddle::operators;
REGISTER_OP_KERNEL(batch_norm,
                   MKLDNN,
                   ::paddle::platform::CPUPlace,
                   ops::BatchNormMKLDNNOpKernel<float>);
REGISTER_OP_KERNEL(batch_norm_grad,
                   MKLDNN,
                   ::paddle::platform::CPUPlace,
                   ops::BatchNormMKLDNNGradOpKernel<float>);
