#include "delegate/hannk_delegate.h"

#include <memory>
#include <string>
#include <vector>

#include "interpreter/interpreter.h"
#include "interpreter/ops.h"
#include "tensorflow/lite/builtin_ops.h"
#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/c/c_api.h"
#include "util/error_util.h"

// TODO: this is likely a worthwhile optimization that we can support without
// too much effort, but requires some testing harnesses we don't have yet
// and isn't likely to be our lowest-hanging fruit. Revisit once other optimizations
// start to become diminishing returns.
#define ALLOW_DYNAMIC_TENSORS 0

// Use a List-Of-X approach here to ensure that places we handle ops are kept in sync
#define ALL_KNOWN_OPS         \
    KNOWN_OP(Add)             \
    KNOWN_OP(AveragePool2d)   \
    KNOWN_OP(Concatenation)   \
    KNOWN_OP(Conv2d)          \
    KNOWN_OP(DepthwiseConv2d) \
    KNOWN_OP(DepthToSpace)    \
    KNOWN_OP(FullyConnected)  \
    KNOWN_OP(L2Normalization) \
    KNOWN_OP(Logistic)        \
    KNOWN_OP(MaxPool2d)       \
    KNOWN_OP(Mean)            \
    KNOWN_OP(Mul)             \
    KNOWN_OP(Pad)             \
    KNOWN_OP(Reshape)         \
    KNOWN_OP(Softmax)         \
    KNOWN_OP(SpaceToDepth)    \
    KNOWN_OP(Sub)             \
    KNOWN_OP(Tanh)

namespace hannk {
namespace {

constexpr char kDelegateName[] = "HannkDelegate";
constexpr int kDelegateVersion = 1;

// -------------------- Some glue code adapted from tfite/c/common.c
int TfLiteIntArrayGetSizeInBytes(int size) {
    static TfLiteIntArray dummy;
    return sizeof(dummy) + sizeof(dummy.data[0]) * size;
}

TfLiteIntArray *TfLiteIntArrayCreate(int size) {
    TfLiteIntArray *ret = (TfLiteIntArray *)malloc(TfLiteIntArrayGetSizeInBytes(size));
    ret->size = size;
    return ret;
}

void TfLiteIntArrayFree(TfLiteIntArray *a) {
    free(a);
}

struct TfLiteIntArrayDeleter {
    void operator()(TfLiteIntArray *a) {
        ::hannk::TfLiteIntArrayFree(a);
    }
};

std::unique_ptr<TfLiteIntArray, TfLiteIntArrayDeleter> BuildTfLiteIntArray(const std::vector<int> &data) {
    std::unique_ptr<TfLiteIntArray, TfLiteIntArrayDeleter> result(TfLiteIntArrayCreate(data.size()));
    std::copy(data.begin(), data.end(), result->data);
    return result;
}

// -------------------- HannkDelegate

struct HannkDelegate final : public TfLiteDelegate {
    explicit HannkDelegate(const HannkDelegateOptions *p)
        : TfLiteDelegate(),
          options_(p != nullptr ? *p : HannkDelegateOptions()) {
        assert(this->data_ == nullptr);
        assert(this->CopyFromBufferHandle == nullptr);
        assert(this->CopyToBufferHandle == nullptr);
        assert(this->FreeBufferHandle == nullptr);
        this->Prepare = DelegatePrepare;
#if ALLOW_DYNAMIC_TENSORS
        this->flags = kTfLiteDelegateFlagsAllowDynamicTensors;
#else
        this->flags = 0;
#endif
    }

    static TfLiteStatus DelegatePrepare(TfLiteContext *context, TfLiteDelegate *delegate);

    const HannkDelegateOptions options_;
};

// -------------------- HannkDelegateKernel

halide_type_t ConvertTfLiteType(TfLiteType t) {
    switch (t) {
    case kTfLiteBool:
        return halide_type_t(halide_type_uint, 1);
    case kTfLiteFloat16:
        return halide_type_t(halide_type_float, 16);
    case kTfLiteFloat32:
        return halide_type_t(halide_type_float, 32);
    case kTfLiteFloat64:
        return halide_type_t(halide_type_float, 64);
    case kTfLiteInt16:
        return halide_type_t(halide_type_int, 16);
    case kTfLiteInt32:
        return halide_type_t(halide_type_int, 32);
    case kTfLiteInt64:
        return halide_type_t(halide_type_int, 64);
    case kTfLiteInt8:
        return halide_type_t(halide_type_int, 8);
    case kTfLiteUInt8:
        return halide_type_t(halide_type_uint, 8);

    case kTfLiteString:
    case kTfLiteComplex64:
    case kTfLiteComplex128:
    default:
        CHECK(0) << "Unhandled type in ConvertTfLiteType";
        return halide_type_t();
    }
}

ActivationFunction ConvertTfLiteActivation(TfLiteFusedActivation a) {
    switch (a) {
    case kTfLiteActNone:
        return ActivationFunction::None;
    case kTfLiteActRelu:
        return ActivationFunction::Relu;
    case kTfLiteActReluN1To1:
        return ActivationFunction::ReluN1To1;
    case kTfLiteActRelu6:
        return ActivationFunction::Relu6;
    case kTfLiteActTanh:
        return ActivationFunction::Tanh;
    case kTfLiteActSignBit:
        return ActivationFunction::SignBit;
    case kTfLiteActSigmoid:
    default:
        CHECK(0) << "Unknown TfLiteFusedActivation";
    }
}

Padding ConvertTfLitePadding(TfLitePadding p) {
    switch (p) {
    case kTfLitePaddingSame:
        return Padding::Same;
    case kTfLitePaddingValid:
        return Padding::Valid;
    default:
        CHECK(0) << "Unknown TfLitePadding";
    }
}

std::vector<int> ConvertTfLiteShape(const TfLiteTensor &tensor) {
    assert(tensor.dims);
    const int shape_size = tensor.dims->size;
    std::vector<int> shape;
    shape.reserve(shape_size);
    for (int i = 0; i < shape_size; i++) {
        shape.push_back(tensor.dims->data[shape_size - 1 - i]);
    }
    return shape;
}

TensorPtr ConvertTfLiteTensor(const TfLiteTensor &tensor) {
    auto shape = ConvertTfLiteShape(tensor);

    halide_type_t type = ConvertTfLiteType(tensor.type);

    QuantizationInfo quantization;
    if (tensor.quantization.type == kTfLiteAffineQuantization) {
        const TfLiteAffineQuantization *q = (const TfLiteAffineQuantization *)tensor.quantization.params;
        for (int i = 0; i < q->scale->size; ++i) {
            const float scale = q->scale->data[i];
            quantization.scale.emplace_back(scale);
        }
        for (int i = 0; i < q->zero_point->size; i++) {
            const int zero = q->zero_point->data[i];
            quantization.zero.emplace_back(zero);
        }
        quantization.dimension = tensor.dims->size - q->quantized_dimension;
    }

    // tensor.name can be null, apparently. I don't think we have any requirement
    // for unique or non-empty names in our code, so let's just map that to
    // an empty string.
    const char *name = tensor.name ? tensor.name : "";

    if (tensor.allocation_type == kTfLiteMmapRo) {
        const void *read_only_data = (const uint8_t *)tensor.data.data;
        assert(read_only_data != nullptr);
        // Construct a HalideBuffer that points to read_only_data (but does not copy or own it).
        // Since TFLite will ensure that the TfLiteTensor remains valid while we're using it,
        // this should be completely safe
        HalideBuffer<void> buffer(type, const_cast<void *>(read_only_data), shape);
        assert(tensor.bytes == buffer.size_in_bytes());

        return std::make_shared<Tensor>(name, std::move(buffer), std::move(quantization));
    }

    // Create an "unallocated" Buffer, which points to null.
    HalideBuffer<void> buffer(type, nullptr, shape);
    return std::make_shared<Tensor>(name, std::move(buffer), std::move(quantization));
}

class HannkDelegateKernel final {
public:
    // Each kernel instance will be used from only a single thread.
    // (It is fine for the kernel itself to use multiple threads internally.)
    explicit HannkDelegateKernel(const HannkDelegateOptions &options)
        : options_(options) {
    }

    // Init() will be called exactly once per instance.
    TfLiteStatus Init(TfLiteContext *context,
                      const TfLiteDelegateParams *params) {
        if (interpreter_ != nullptr) {
            TF_LITE_KERNEL_LOG(context, "Init must not be called twice.");
            return kTfLiteError;
        }

        std::vector<int> node_indices(params->nodes_to_replace->size);
        for (int i = 0; i < params->nodes_to_replace->size; i++) {
            const int node_index = params->nodes_to_replace->data[i];
            node_indices[i] = node_index;
        }
        if (options_.verbosity >= 1) {
            LOG(INFO) << "Delegate " << (void *)this << " Init nodes: " << node_indices << "\n";
        }

        // Pre-emptively map *all* the TFLiteTensors into our Tensor type.
        for (size_t tensor_id = 0; tensor_id < context->tensors_size; tensor_id++) {
            const TfLiteTensor &tensor = context->tensors[tensor_id];
            if (tensor.dims == nullptr) {
                // Can't convert a TfLiteTensor with no dimension info
                continue;
            }
            auto t = ConvertTfLiteTensor(tensor);
            assert(!tensors_.count(tensor_id));
            tensors_[tensor_id] = t;
            if (options_.verbosity >= 1) {
                LOG(INFO) << "tensor_id " << tensor_id << " -> " << (void *)t.get() << "\n";
            }
        }

        // Be careful with params->input_tensors and params->output_tensors here;
        // in particular, params->input_tensors will contain all of the 'constant'
        // input tensors (which are generally inputs only to a specific node).
#if ALLOW_DYNAMIC_TENSORS
        // TODO: verify the above comment is still correct.
#endif

        // Mark the input and output tensors correctly, as code in our interpreter
        // relies upon it.
        std::vector<TensorPtr> inputs;
        for (int i = 0; i < params->input_tensors->size; i++) {
            const int tensor_id = params->input_tensors->data[i];
            if (tensor_id == kTfLiteOptionalTensor) {
                continue;
            }
            auto t = GetTensorById(context, tensor_id);
            t->set_input(true);
            inputs.push_back(t);
            if (options_.verbosity >= 2) {
                LOG(INFO) << "Delegate " << (void *)this << (t->is_constant() ? " Const" : "") << " Input tensor: " << tensor_id << "\n";
            }
        }

        // Add the output tensors.
        std::vector<TensorPtr> outputs;
        for (int i = 0; i < params->output_tensors->size; i++) {
            const int tensor_id = params->output_tensors->data[i];
            if (tensor_id == kTfLiteOptionalTensor) {
                continue;
            }
            if (options_.verbosity >= 2) {
                LOG(INFO) << "Delegate " << (void *)this << " Output tensor: " << tensor_id << "\n";
            }
            auto t = GetTensorById(context, tensor_id);
            t->set_output(true);
            outputs.push_back(t);
        }

        // Add all ops.
        TfLiteNode *node;
        TfLiteRegistration *reg;
        std::vector<std::unique_ptr<Op>> ops;
        for (int node_index : node_indices) {
            TF_LITE_ENSURE_STATUS(context->GetNodeAndRegistration(context, node_index, &node, &reg));
            const int op_type = reg->builtin_code;
            std::unique_ptr<Op> op;

            // clang-format off
            switch (op_type) {
                #define KNOWN_OP(OP) case kTfLiteBuiltin##OP: op = Build##OP(context, node); break;
                ALL_KNOWN_OPS
                #undef KNOWN_OP

            default:
                TF_LITE_KERNEL_LOG(context, "Op not supported: %d", op_type);
                return kTfLiteError;
            }
            // clang-format on

            if (op == nullptr) {
                TF_LITE_KERNEL_LOG(context, "Op factory returned null: %s", op_type);
                return kTfLiteError;
            }
            ops.push_back(std::move(op));
        }
        model_ = ::hannk::make_unique<OpGroup>(std::move(inputs), std::move(outputs), std::move(ops));

        return kTfLiteOk;
    }

    // Prepare() will be called at least once, prior to any calls to Eval().
    // It will be called again if tensor shape(s) change. It is preferable
    // to do all memory allocation in Prepare(), rather than Eval(), if possible.
    TfLiteStatus Prepare(TfLiteContext *context, TfLiteNode *node) {
        if (options_.verbosity >= 1) {
            LOG(INFO) << "Delegate " << (void *)this << " Prepare\n";
        }

        assert(model_ != nullptr);

#if ALLOW_DYNAMIC_TENSORS
        // Because we set kTfLiteDelegateFlagsAllowDynamicTensors, TFLite
        // may call Prepare() after Eval() if only tensor shapes have changed
        // (but nothing else in the model), which is a nice potential optimization.
        // (Apparently, if you don't set kTfLiteDelegateFlagsAllowDynamicTensors,
        // TFLite will create a fresh Delegate for every call instead.)
        //
        // TODO: will be called with interp (but no model) if inputs resized.
        // update the tensors in the model/interp.
        abort();  // TODO
#else
        if (interpreter_ != nullptr) {
            TF_LITE_KERNEL_LOG(context, "Calling Prepare() multiple times");
            return kTfLiteError;
        }
#endif

        interpreter_ = ::hannk::make_unique<Interpreter>(std::move(model_));
        return kTfLiteOk;
    }

    // Eval() will be called at least once. It can expect that prepare() will
    // have been called for the current set of tensor shape(s).
    TfLiteStatus Eval(TfLiteContext *context, TfLiteNode *node) {
        if (interpreter_ == nullptr) {
            TF_LITE_KERNEL_LOG(context, "interpreter_ is not built in Eval");
            return kTfLiteError;
        }

        // Copy the non-constant Tensor inputs. TODO: avoid this by sharing pointers.
        for (int i = 0; i < node->inputs->size; i++) {
            const int tensor_id = node->inputs->data[i];
            if (tensor_id == kTfLiteOptionalTensor) {
                continue;
            }
            assert(tensor_id >= 0 && tensor_id < (int)context->tensors_size);
            const TfLiteTensor &tensor = context->tensors[tensor_id];
            auto t = GetTensorById(context, tensor_id);
            assert(t->is_constant() == (tensor.allocation_type == kTfLiteMmapRo));
            if (t->is_constant()) {
                continue;
            }
            assert(t->is_input() && !t->is_constant() && t->is_allocated());
            auto buf = t->buffer();
            assert(buf.size_in_bytes() == tensor.bytes);

            memcpy(buf.data(), tensor.data.data, tensor.bytes);
        }

        // TODO: execute needs to return an error code.
        interpreter_->execute();

        // Copy the Tensor outputs. TODO: avoid this by sharing pointers.
        for (int i = 0; i < node->outputs->size; i++) {
            const int tensor_id = node->outputs->data[i];
            if (tensor_id == kTfLiteOptionalTensor) {
                continue;
            }
            assert(tensor_id >= 0 && tensor_id < (int)context->tensors_size);
            const TfLiteTensor &tensor = context->tensors[tensor_id];
            assert(tensor.allocation_type != kTfLiteMmapRo);
            auto t = GetTensorById(context, tensor_id);
            assert(t->is_output() && !t->is_constant() && t->is_allocated());
            auto buf = t->buffer();
            assert(buf.size_in_bytes() == tensor.bytes);

            memcpy(tensor.data.data, buf.data(), tensor.bytes);
        }

        // Eval() could be called again with the same graph -- don't destroy the interpreter_ yet.

        return kTfLiteOk;
    }

    static TfLiteRegistration GetRegistration() {
        TfLiteRegistration r{};
        r.init = InitImpl;
        r.free = FreeImpl;
        r.prepare = PrepareImpl;
        r.invoke = InvokeImpl;
        r.profiling_string = nullptr;
        r.builtin_code = kTfLiteBuiltinDelegate;
        r.custom_name = kDelegateName;
        r.version = kDelegateVersion;
        return r;
    }

private:
    static void *InitImpl(TfLiteContext *context, const char *buffer, size_t length) {
        const TfLiteDelegateParams *params = (const TfLiteDelegateParams *)buffer;
        if (params == nullptr) {
            LOG(ERROR) << "HannkDelegate.init: NULL params";
            return nullptr;
        }
        HannkDelegate *hannk_delegate = (HannkDelegate *)params->delegate;
        auto self = ::hannk::make_unique<HannkDelegateKernel>(hannk_delegate->options_);
        if (self->Init(context, params) != kTfLiteOk) {
            LOG(ERROR) << "HannkDelegate.init: NULL params";
            return nullptr;
        }
        return self.release();
    };

    static void FreeImpl(TfLiteContext *context, void *buffer) {
        HannkDelegateKernel *self = (HannkDelegateKernel *)buffer;
        delete self;
    };

    static TfLiteStatus PrepareImpl(TfLiteContext *context, TfLiteNode *node) {
        if (node->user_data == nullptr) {
            LOG(ERROR) << "Delegate kernel was not initialized";
            return kTfLiteError;
        }
        HannkDelegateKernel *self = (HannkDelegateKernel *)node->user_data;
        return self->Prepare(context, node);
    };

    static TfLiteStatus InvokeImpl(TfLiteContext *context, TfLiteNode *node) {
        HannkDelegateKernel *self = (HannkDelegateKernel *)node->user_data;
        assert(self != nullptr);
        return self->Eval(context, node);
    };

    TensorPtr GetTensorById(TfLiteContext *context, int tensor_id) {
        auto it = tensors_.find(tensor_id);
        if (it == tensors_.end()) {
            LOG(ERROR) << "tensor_id not found: " << tensor_id;
            return nullptr;
        }
        return it->second;
    }

    std::unique_ptr<Op> BuildAdd(TfLiteContext *context, TfLiteNode *node) {
        auto input1 = GetTensorById(context, node->inputs->data[0]);
        auto input2 = GetTensorById(context, node->inputs->data[1]);
        auto output = GetTensorById(context, node->outputs->data[0]);
        const TfLiteAddParams *params = (const TfLiteAddParams *)(node->builtin_data);
        auto activation = ConvertTfLiteActivation(params->activation);
        return ::hannk::make_unique<BinaryOp>(input1, input2, output, BinaryOp::Add, activation);
    }

    std::unique_ptr<Op> BuildSub(TfLiteContext *context, TfLiteNode *node) {
        auto input1 = GetTensorById(context, node->inputs->data[0]);
        auto input2 = GetTensorById(context, node->inputs->data[1]);
        auto output = GetTensorById(context, node->outputs->data[0]);
        const TfLiteSubParams *params = (const TfLiteSubParams *)(node->builtin_data);
        auto activation = ConvertTfLiteActivation(params->activation);
        return ::hannk::make_unique<BinaryOp>(input1, input2, output, BinaryOp::Sub, activation);
    }

    std::unique_ptr<Op> BuildMul(TfLiteContext *context, TfLiteNode *node) {
        auto input1 = GetTensorById(context, node->inputs->data[0]);
        auto input2 = GetTensorById(context, node->inputs->data[1]);
        auto output = GetTensorById(context, node->outputs->data[0]);
        const TfLiteMulParams *params = (const TfLiteMulParams *)(node->builtin_data);
        auto activation = ConvertTfLiteActivation(params->activation);
        return ::hannk::make_unique<BinaryOp>(input1, input2, output, BinaryOp::Mul, activation);
    }

    std::unique_ptr<Op> BuildPool2d(TfLiteContext *context, TfLiteNode *node, PoolOp::Operator reduce_op) {
        auto input = GetTensorById(context, node->inputs->data[0]);
        auto output = GetTensorById(context, node->outputs->data[0]);
        const TfLitePoolParams *params = (const TfLitePoolParams *)(node->builtin_data);
        auto padding = ConvertTfLitePadding(params->padding);
        const std::vector<int> stride = {
            params->stride_width,
            params->stride_height,
        };
        const std::vector<int> filter_size = {
            params->filter_width,
            params->filter_height,
        };
        auto activation = ConvertTfLiteActivation(params->activation);
        return ::hannk::make_unique<PoolOp>(input, output, stride, filter_size, padding, reduce_op, activation);
    }

    std::unique_ptr<Op> BuildAveragePool2d(TfLiteContext *context, TfLiteNode *node) {
        return BuildPool2d(context, node, PoolOp::Average);
    }

    std::unique_ptr<Op> BuildMaxPool2d(TfLiteContext *context, TfLiteNode *node) {
        return BuildPool2d(context, node, PoolOp::Max);
    }

    std::unique_ptr<Op> BuildConcatenation(TfLiteContext *context, TfLiteNode *node) {
        std::vector<TensorPtr> inputs(node->inputs->size);
        for (int i = 0; i < node->inputs->size; i++) {
            inputs[i] = GetTensorById(context, node->inputs->data[i]);
        }
        auto output = GetTensorById(context, node->outputs->data[0]);
        const TfLiteConcatenationParams *params = (const TfLiteConcatenationParams *)(node->builtin_data);
        auto activation = ConvertTfLiteActivation(params->activation);
        CHECK(activation == ActivationFunction::None);
        int axis = params->axis;
        // Handle negative values, which are legal
        if (axis < 0) {
            axis = (int)output->rank() + axis;
        }
        // Now 'flip' the axis so that it refers to the right dimension in
        // the Tensor (since we reverse the dimension order)
        axis = (int)output->rank() - axis - 1;
        return ::hannk::make_unique<ConcatenationOp>(inputs, output, axis);
    }

    std::unique_ptr<Op> BuildConv2d(TfLiteContext *context, TfLiteNode *node) {
        auto input = GetTensorById(context, node->inputs->data[0]);
        auto filter = GetTensorById(context, node->inputs->data[1]);
        auto bias = GetTensorById(context, node->inputs->data[2]);
        auto output = GetTensorById(context, node->outputs->data[0]);
        const TfLiteConvParams *params = (const TfLiteConvParams *)(node->builtin_data);
        auto padding = ConvertTfLitePadding(params->padding);
        const std::vector<int> stride = {
            params->stride_width,
            params->stride_height,
        };
        const std::vector<int> dilation_factor = {
            params->dilation_width_factor,
            params->dilation_height_factor,
        };
        auto activation = ConvertTfLiteActivation(params->activation);
        return ::hannk::make_unique<Conv2DOp>(input, filter, bias, output, stride,
                                              dilation_factor, padding, activation);
    }

    std::unique_ptr<Op> BuildDepthwiseConv2d(TfLiteContext *context, TfLiteNode *node) {
        auto input = GetTensorById(context, node->inputs->data[0]);
        auto filter = GetTensorById(context, node->inputs->data[1]);
        auto bias = GetTensorById(context, node->inputs->data[2]);
        auto output = GetTensorById(context, node->outputs->data[0]);
        const TfLiteDepthwiseConvParams *params = (const TfLiteDepthwiseConvParams *)(node->builtin_data);
        int depth_multiplier = output->extent(0) / input->extent(0);
        const std::vector<int> stride = {
            params->stride_width,
            params->stride_height,
        };
        const std::vector<int> dilation_factor = {
            params->dilation_width_factor,
            params->dilation_height_factor,
        };
        auto padding = ConvertTfLitePadding(params->padding);
        auto activation = ConvertTfLiteActivation(params->activation);
        return ::hannk::make_unique<DepthwiseConv2DOp>(input, filter, bias, output, depth_multiplier,
                                                       stride, dilation_factor, padding, activation);
    }

    std::unique_ptr<Op> BuildFullyConnected(TfLiteContext *context, TfLiteNode *node) {
        auto input = GetTensorById(context, node->inputs->data[0]);
        auto filter = GetTensorById(context, node->inputs->data[1]);
        auto bias = GetTensorById(context, node->inputs->data[2]);
        auto output = GetTensorById(context, node->outputs->data[0]);
        const TfLiteFullyConnectedParams *params = (const TfLiteFullyConnectedParams *)(node->builtin_data);
        auto activation = ConvertTfLiteActivation(params->activation);
        return ::hannk::make_unique<FullyConnectedOp>(input, filter, bias, output, activation);
    }

    std::unique_ptr<Op> BuildPad(TfLiteContext *context, TfLiteNode *node) {
        auto input = GetTensorById(context, node->inputs->data[0]);
        auto padding = GetTensorById(context, node->inputs->data[1]);
        auto output = GetTensorById(context, node->outputs->data[0]);
        return ::hannk::make_unique<PadOp>(input, padding, output);
    }

    std::unique_ptr<Op> BuildReshape(TfLiteContext *context, TfLiteNode *node) {
        auto input = GetTensorById(context, node->inputs->data[0]);
        auto output = GetTensorById(context, node->outputs->data[0]);
        const TfLiteReshapeParams *params = (const TfLiteReshapeParams *)(node->builtin_data);
        std::vector<int> new_shape;
        new_shape.assign(params->shape, params->shape + params->num_dimensions);
        return ::hannk::make_unique<ReshapeOp>(input, output, new_shape);
    }

    std::unique_ptr<Op> BuildSoftmax(TfLiteContext *context, TfLiteNode *node) {
        auto input = GetTensorById(context, node->inputs->data[0]);
        auto output = GetTensorById(context, node->outputs->data[0]);
        const TfLiteSoftmaxParams *params = (const TfLiteSoftmaxParams *)(node->builtin_data);
        return ::hannk::make_unique<SoftmaxOp>(input, output, params->beta);
    }

    std::unique_ptr<Op> BuildL2Normalization(TfLiteContext *context, TfLiteNode *node) {
        auto input = GetTensorById(context, node->inputs->data[0]);
        auto output = GetTensorById(context, node->outputs->data[0]);
        return ::hannk::make_unique<L2NormalizationOp>(input, output);
    }

    std::unique_ptr<Op> BuildLogistic(TfLiteContext *context, TfLiteNode *node) {
        auto input = GetTensorById(context, node->inputs->data[0]);
        auto output = GetTensorById(context, node->outputs->data[0]);
        return ::hannk::make_unique<UnaryOp>(input, output, UnaryOp::Logistic);
    }

    std::unique_ptr<Op> BuildTanh(TfLiteContext *context, TfLiteNode *node) {
        auto input = GetTensorById(context, node->inputs->data[0]);
        auto output = GetTensorById(context, node->outputs->data[0]);
        return ::hannk::make_unique<UnaryOp>(input, output, UnaryOp::Tanh);
    }

    std::unique_ptr<Op> BuildMean(TfLiteContext *context, TfLiteNode *node) {
        auto input = GetTensorById(context, node->inputs->data[0]);
        auto indices = GetTensorById(context, node->inputs->data[1]);
        auto output = GetTensorById(context, node->outputs->data[0]);
        return ::hannk::make_unique<ReductionOp>(input, indices, output, ReductionOp::Mean);
    }

    std::unique_ptr<Op> BuildSpaceToDepth(TfLiteContext *context, TfLiteNode *node) {
        auto input = GetTensorById(context, node->inputs->data[0]);
        auto output = GetTensorById(context, node->outputs->data[0]);
        const TfLiteSpaceToDepthParams *params = (const TfLiteSpaceToDepthParams *)(node->builtin_data);
        return ::hannk::make_unique<SpaceDepthOp>(input, output, params->block_size);
    }

    std::unique_ptr<Op> BuildDepthToSpace(TfLiteContext *context, TfLiteNode *node) {
        auto input = GetTensorById(context, node->inputs->data[0]);
        auto output = GetTensorById(context, node->outputs->data[0]);
        const TfLiteDepthToSpaceParams *params = (const TfLiteDepthToSpaceParams *)(node->builtin_data);
        return ::hannk::make_unique<SpaceDepthOp>(input, output, -params->block_size);
    }

    const HannkDelegateOptions options_;
    std::unique_ptr<OpGroup> model_;
    std::unique_ptr<Interpreter> interpreter_;
    // TODO: unordered_map might be a better choice.
    std::map<int, TensorPtr> tensors_;
};

bool InputsHaveCorrectTypes(const TfLiteNode *node,
                            TfLiteContext *context,
                            std::initializer_list<int> per_input_possible_types_mask) {
    if (node->inputs->size != (int)per_input_possible_types_mask.size()) {
        LOG(ERROR) << "inputs size mismatch in InputsHaveCorrectTypes";
        return false;
    }
    int i = -1;
    for (int possible_types_mask : per_input_possible_types_mask) {
        ++i;
        // Skip optional tensor.
        const int tensor_id = node->inputs->data[i];
        if (tensor_id == kTfLiteOptionalTensor) {
            continue;
        }
        const TfLiteTensor &tensor = context->tensors[tensor_id];
        const int tensor_type_mask = 1 << tensor.type;
        if (!(tensor_type_mask & possible_types_mask)) {
            return false;
        }
    }
    return true;
}

bool AllInputsHaveType(const TfLiteNode *node, TfLiteContext *context, int possible_types_mask) {
    for (int i = 0; i < node->inputs->size; ++i) {
        const int tensor_id = node->inputs->data[i];
        if (tensor_id == kTfLiteOptionalTensor) {
            continue;
        }
        const TfLiteTensor &tensor = context->tensors[tensor_id];
        const int tensor_type_mask = 1 << tensor.type;
        if (!(tensor_type_mask & possible_types_mask)) {
            return false;
        }
    }
    return true;
}

bool IsActivationReluOrNone(TfLiteFusedActivation activation) {
    return (activation == kTfLiteActRelu ||
            activation == kTfLiteActRelu6 ||
            activation == kTfLiteActReluN1To1 ||
            activation == kTfLiteActNone);
}

// TODO: this should also allow Int8 once we fix biasing for those
constexpr int k8BitMask = 1 << kTfLiteUInt8;

bool IsNodeSupported_Add(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    if (!(registration->version <= 2)) {
        return false;
    }
    if (!InputsHaveCorrectTypes(node, context, {k8BitMask, k8BitMask})) {
        return false;
    }
    const TfLiteAddParams *params = (const TfLiteAddParams *)(node->builtin_data);
    if (!IsActivationReluOrNone(params->activation)) {
        return false;
    }
    return true;
}

bool IsNodeSupported_Sub(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    return IsNodeSupported_Add(context, node, registration);
}

bool IsNodeSupported_Mul(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    return IsNodeSupported_Add(context, node, registration);
}

bool IsNodeSupported_Concatenation(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    if (!(registration->version <= 2)) {
        return false;
    }
    if (!AllInputsHaveType(node, context, k8BitMask)) {
        return false;
    }
    // TODO: This op has an activation but we don't appear to use it.
    // const TfLiteConcatenationParams *params = (const TfLiteConcatenationParams *)(node->builtin_data);
    return true;
}

bool IsNodeSupported_Conv2d(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    if (!(registration->version <= 2)) {
        return false;
    }
    if (!InputsHaveCorrectTypes(node, context, {k8BitMask, k8BitMask, 1 << kTfLiteInt32})) {
        return false;
    }
    const TfLiteConvParams *params = (const TfLiteConvParams *)(node->builtin_data);
    if (!IsActivationReluOrNone(params->activation)) {
        return false;
    }
    return true;
}

bool IsNodeSupported_DepthwiseConv2d(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    if (!(registration->version <= 2)) {
        return false;
    }
    if (!InputsHaveCorrectTypes(node, context, {k8BitMask, k8BitMask, 1 << kTfLiteInt32})) {
        return false;
    }
    const TfLiteDepthwiseConvParams *params = (const TfLiteDepthwiseConvParams *)(node->builtin_data);
    if (!IsActivationReluOrNone(params->activation)) {
        return false;
    }
    return true;
}

bool IsNodeSupported_FullyConnected(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    // This is correct, we don't handle the params for v2 or later yet
    if (!(registration->version <= 1)) {
        return false;
    }
    if (!InputsHaveCorrectTypes(node, context, {k8BitMask, k8BitMask, (1 << kTfLiteInt32) | (1 << kTfLiteNoType)})) {
        return false;
    }
    const TfLiteFullyConnectedParams *params = (const TfLiteFullyConnectedParams *)(node->builtin_data);
    if (!IsActivationReluOrNone(params->activation)) {
        return false;
    }
    return true;
}

bool IsNodeSupported_Pool2d(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    if (!(registration->version <= 2)) {
        return false;
    }
    if (!InputsHaveCorrectTypes(node, context, {k8BitMask})) {
        return false;
    }
    const TfLitePoolParams *params = (const TfLitePoolParams *)(node->builtin_data);
    if (!IsActivationReluOrNone(params->activation)) {
        return false;
    }
    return true;
}

bool IsNodeSupported_AveragePool2d(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    return IsNodeSupported_Pool2d(context, node, registration);
}

bool IsNodeSupported_MaxPool2d(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    return IsNodeSupported_Pool2d(context, node, registration);
}

bool IsNodeSupported_Pad(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    if (!(registration->version <= 2)) {
        return false;
    }
    if (!InputsHaveCorrectTypes(node, context, {k8BitMask, 1 << kTfLiteInt32})) {
        return false;
    }
    return true;
}

bool IsNodeSupported_Reshape(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    if (!(registration->version <= 2)) {
        return false;
    }
    // Note that Reshape can have 1 or 2 inputs.
    if (node->inputs->size > 2) {
        return false;
    }
    return true;
}

bool IsNodeSupported_Softmax(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    if (!(registration->version <= 2)) {
        return false;
    }
    if (!InputsHaveCorrectTypes(node, context, {k8BitMask})) {
        return false;
    }
    return true;
}

bool IsNodeSupported_L2Normalization(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    if (!(registration->version <= 2)) {
        return false;
    }
    if (!InputsHaveCorrectTypes(node, context, {k8BitMask})) {
        return false;
    }
    return true;
}

bool IsNodeSupported_Logistic(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    if (!(registration->version <= 2)) {
        return false;
    }
    if (!InputsHaveCorrectTypes(node, context, {k8BitMask})) {
        return false;
    }
    return true;
}

bool IsNodeSupported_Tanh(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    if (!(registration->version <= 2)) {
        return false;
    }
    if (!InputsHaveCorrectTypes(node, context, {k8BitMask})) {
        return false;
    }
    return true;
}

bool IsNodeSupported_Mean(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    if (!(registration->version <= 2)) {
        return false;
    }
    if (!InputsHaveCorrectTypes(node, context, {k8BitMask, 1 << kTfLiteInt32})) {
        return false;
    }
    return true;
}

bool IsNodeSupported_SpaceToDepth(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    if (!(registration->version <= 2)) {
        return false;
    }
    if (!InputsHaveCorrectTypes(node, context, {k8BitMask})) {
        return false;
    }
    return true;
}

bool IsNodeSupported_DepthToSpace(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    if (!(registration->version <= 2)) {
        return false;
    }
    if (!InputsHaveCorrectTypes(node, context, {k8BitMask})) {
        return false;
    }
    return true;
}

bool IsNodeSupported(TfLiteContext *context, TfLiteNode *node, TfLiteRegistration *registration) {
    // Ensure all inputs & outputs have dim <= 4.
    for (int i = 0; i < node->inputs->size; ++i) {
        const int tensor_id = node->inputs->data[i];
        if (tensor_id == kTfLiteOptionalTensor) {
            continue;
        }
        const TfLiteTensor &tensor = context->tensors[tensor_id];
        assert(tensor.dims);
        if (tensor.dims->size > 4) {
            return false;
        }
    }
    for (int i = 0; i < node->outputs->size; ++i) {
        const int tensor_id = node->outputs->data[i];
        const TfLiteTensor &tensor = context->tensors[tensor_id];
        assert(tensor.dims);
        if (tensor.dims->size > 4) {
            return false;
        }
    }

    // Now check for each specific node.
    //
    // TODO: Our existing code for TFLiteParser, etc doesn't pay
    // attention to version (AFAICT); need to find & examine the specs of
    // version changes to ensure this is correct. Existing version checking
    // here is mostly bogus. See tensorflow/lite/tools/versioning/op_version.cc
    //
    // TODO: style here is imitation of approach used in Hexagon delegate,
    // but a purely data-table-driven-approach might be better in the long run?

    // clang-format off
    switch (registration->builtin_code) {

        #define KNOWN_OP(OP) case kTfLiteBuiltin##OP: return IsNodeSupported_##OP(context, node, registration);
        ALL_KNOWN_OPS
        #undef KNOWN_OP

    default:
        return false;
    }
    // clang-format on

    return false;
}

/*static*/ TfLiteStatus HannkDelegate::DelegatePrepare(TfLiteContext *context, TfLiteDelegate *delegate) {
    TfLiteStatus status;

    TfLiteIntArray *plan = nullptr;
    if ((status = context->GetExecutionPlan(context, &plan)) != kTfLiteOk) {
        LOG(ERROR) << "GetExecutionPlan failed";
        return status;
    }

    // Build up a list of the nodes we want to handle.
    std::vector<int> supported_nodes;
    for (int i = 0; i < plan->size; i++) {
        const int node_index = plan->data[i];
        TfLiteNode *node;
        TfLiteRegistration *registration;
        if ((status = context->GetNodeAndRegistration(context, node_index, &node, &registration)) != kTfLiteOk) {
            LOG(ERROR) << "GetNodeAndRegistration failed";
            return status;
        }

        if (IsNodeSupported(context, node, registration)) {
            supported_nodes.push_back(node_index);
        } else {
            // TODO: consider using a lambda to pass in the options_ struct
            // so we can gate this via verbosity.
            //
            // NOTE: The TFLite C API doesn't provide a way to map builtin_code
            // to a readable name; see lite/builtin_ops.h to find what sort
            // of node(s) we are skipping here. (The names are available if
            // we add a dependency on the generated schema file, but that's a
            // dep we don't otherwise need or want here.)
            LOG(INFO) << "Skipping unsupported node, index=" << node_index
                      << " code=" << registration->builtin_code
                      << " custom_name=(" << (registration->custom_name ? registration->custom_name : "nullptr") << ")"
                      << "\n";
        }
    }

    if ((status = context->ReplaceNodeSubsetsWithDelegateKernels(context,
                                                                 HannkDelegateKernel::GetRegistration(),
                                                                 BuildTfLiteIntArray(supported_nodes).get(),
                                                                 delegate)) != kTfLiteOk) {
        LOG(ERROR) << "ReplaceNodeSubsetsWithDelegateKernels failed";
        return status;
    }

    return kTfLiteOk;
}

}  // namespace
}  // namespace hannk

TfLiteDelegate *HannkDelegateCreate(const HannkDelegateOptions *options) {
    using hannk::HannkDelegate;

    return new HannkDelegate(options);
}

void HannkDelegateOptionsDefault(HannkDelegateOptions *opt) {
    *opt = HannkDelegateOptions();
}

void HannkDelegateDelete(TfLiteDelegate *delegate) {
    using hannk::HannkDelegate;

    if (delegate) {
        HannkDelegate *hannk_delegate = (HannkDelegate *)delegate;
        delete hannk_delegate;
    }
}