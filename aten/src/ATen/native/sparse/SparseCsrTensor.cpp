// Basic functions on sparse tensors

#include <ATen/ATen.h>
#include <ATen/InitialTensorOptions.h>
#include <ATen/Layout.h>
#include <ATen/NativeFunctions.h>
#include <ATen/Parallel.h>
#include <ATen/SparseCsrTensorImpl.h>
#include <ATen/SparseCsrTensorUtils.h>
#include <ATen/SparseTensorImpl.h>
#include <ATen/InitialTensorOptions.h>

namespace at {
namespace native {

using namespace at::sparse_csr;

namespace {


} // end anonymous namespace

void _validate_sparse_csr_tensor_args(const Tensor& crow_indices, const Tensor& col_indices, const Tensor& values, IntArrayRef size) {
  // Layout Invariants
  TORCH_CHECK(
      col_indices.layout() == kStrided && col_indices.is_contiguous(),
      "expected col_indices to be a strided and contiguous tensor");

  TORCH_CHECK(
      crow_indices.layout() == kStrided && crow_indices.is_contiguous(),
      "expected crow_indices to be a strided and contiguous tensor");

  TORCH_CHECK(
      values.layout() == kStrided && values.is_contiguous(),
      "expected values to be a strided and contiguous tensor");

  // Shape and Strides invariants
  TORCH_CHECK(
      size.size() == 2,
      "size of a CSR tensor must be of length 2, but got: ",
      size.size());
  TORCH_CHECK(
      crow_indices.dim() == 1,
      "crow_indices must have dim=1 but got crow_indices.dim()=",
      crow_indices.dim());
  TORCH_CHECK(
      col_indices.dim() == 1,
      "col_indices must have dim=1 but got col_indices.dim()=",
      col_indices.dim());
  TORCH_CHECK(
      values.dim() == 1,
      "values must have dim=1 but got values.dim()=",
      values.dim());
  // Note, this check also enforces `crow_indices.numel() >= 1`
  TORCH_CHECK(
      crow_indices.numel() == (size[0] + 1),
      "crow_indices.numel() must be size(0) + 1, but got: ",
      crow_indices.numel());
  TORCH_CHECK(
      col_indices.numel() == values.numel(),
      "col_indices and values must have equal sizes, but got col_indices.numel(): ",
      col_indices.numel(),
      ", values.numel(): ",
      values.numel());

  // Indices invariants
  AT_DISPATCH_INDEX_TYPES(crow_indices.scalar_type(), "csr_construct_check", [&] {
    Tensor crow_indices_cpu = crow_indices.to(kCPU);
    auto crow_indices_accessor = crow_indices_cpu.accessor<index_t, 1>();
    TORCH_CHECK(
        crow_indices_accessor[0] == 0, "0th value of crow_indices must be 0.");

    TORCH_CHECK(
        crow_indices_accessor[crow_indices.numel() - 1] == col_indices.numel(),
        "last value of crow_indices should be equal to the length of col_indices.");

    for (int i =  1; i <= size[0]; i++) {
      TORCH_CHECK(
          crow_indices_accessor[i - 1] <= crow_indices_accessor[i],
          "at position i = ", i, ", this condition crow_indices[i - 1] <= crow_indices[i] fails");
    }
    if (col_indices.numel() > 0) {
      TORCH_CHECK(0 <= col_indices.min().item<index_t>(), "col_indices.min() should be greater or equal to zero");
      TORCH_CHECK(size[1] > col_indices.max().item<index_t>(), "size(1) should be greater than col_indices.max()");
    }
  });
}

// Construction of CSR tensors.
SparseCsrTensor new_csr_tensor(const TensorOptions& options) {
  // TODO: remove this comment after enabling autograd support for CSR tensor
  // constructor.
  // TORCH_INTERNAL_ASSERT(impl::variable_excluded_from_dispatch());
  TORCH_INTERNAL_ASSERT(options.layout() == kSparseCsr);
  DispatchKey dispatch_key;

  if (options.device().is_cuda()) {
    dispatch_key = DispatchKey::SparseCsrCUDA;
  } else {
    TORCH_INTERNAL_ASSERT(options.device().is_cpu());
    dispatch_key = DispatchKey::SparseCsrCPU;
  }

  return detail::make_tensor<SparseCsrTensorImpl>(
      DispatchKeySet(dispatch_key), options.dtype());
}

Tensor _sparse_csr_tensor_unsafe(const Tensor& crow_indices, const Tensor& col_indices,
    const Tensor& values,
    IntArrayRef size,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory) {

  TensorOptions options = TensorOptions().dtype(dtype).layout(layout).device(device).pinned_memory(pin_memory);

  TORCH_CHECK_NOT_IMPLEMENTED(
    options.device().type() == kCPU || options.device().type() == kCUDA,
     "Could not run '", "sparse_csr_tensor", "' from the '", options.device(), "' device.)");

  TORCH_CHECK(
      options.layout() == kSparseCsr,
      "expected sparse CSR layout, but got layout ",
      options.layout());

  SparseCsrTensor self = new_csr_tensor(options);
  get_sparse_csr_impl(self)->set_member_tensors(crow_indices, col_indices, values, size);
  return self;
}

// TODO: This constructor should probably use an ATen abstract method in order
// to make autograd dispatch available for the CSR constructor. See the relevant
// note in native_functions.yaml.
Tensor sparse_csr_tensor(
    const Tensor& crow_indices,
    const Tensor& col_indices,
    const Tensor& values,
    IntArrayRef size,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory) {
  // See [Note: hacky wrapper removal for TensorOptions]
  TensorOptions options = TensorOptions().dtype(dtype).layout(layout).device(device).pinned_memory(pin_memory);

  at::native::_validate_sparse_csr_tensor_args(crow_indices, col_indices, values, size);

  return at::native::_sparse_csr_tensor_unsafe(
      crow_indices,
      col_indices,
      values,
      size,
      optTypeMetaToScalarType(options.dtype_opt()),
      options.layout_opt(),
      options.device_opt(),
      options.pinned_memory_opt());
}

Tensor sparse_csr_tensor(
    const Tensor& crow_indices,
    const Tensor& col_indices,
    const Tensor& values,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory) {
  // See [Note: hacky wrapper removal for TensorOptions]
  TensorOptions options = TensorOptions().dtype(dtype).layout(layout).device(device).pinned_memory(pin_memory);
  std::array<int64_t, 2> size;
  if (col_indices.numel() > 0) {
    AT_DISPATCH_INDEX_TYPES(col_indices.scalar_type(), "csr_construct_check", [&] {
      size[0] = crow_indices.numel() - 1;
      size[1] = col_indices.max().item<index_t>() + 1;
    });
  } else {
    size[0] = 0;
    size[1] = 0;
  }

  at::native::_validate_sparse_csr_tensor_args(crow_indices, col_indices, values, size);

  return at::native::_sparse_csr_tensor_unsafe(
      crow_indices,
      col_indices,
      values,
      size,
      optTypeMetaToScalarType(options.dtype_opt()),
      options.layout_opt(),
      options.device_opt(),
      options.pinned_memory_opt());
}

// Access members of CSR tensors.
int64_t _nnz_sparse_csr(const SparseCsrTensor& self) {
  return get_sparse_csr_impl(self)->nnz();
}

Tensor values_sparse_csr(const Tensor& self) {
  return get_sparse_csr_impl(self)->values().alias();
}

Tensor crow_indices_sparse_csr(const Tensor& self) {
  return get_sparse_csr_impl(self)->crow_indices().alias();
}

Tensor col_indices_sparse_csr(const Tensor& self) {
  return get_sparse_csr_impl(self)->col_indices().alias();
}

bool _is_same_size_as_sparse_csr(
    const SparseCsrTensor& self,
    const SparseCsrTensor& src) {
  return self.sizes().equals(src.sizes());
}

const SparseCsrTensor& resize_as_sparse_csr_(
    const SparseCsrTensor& self,
    const SparseCsrTensor& src) {
  TORCH_CHECK(
      src.is_sparse_csr() && self.is_sparse_csr(),
      "resize_as_sparse_csr_: layout for self and src must be sparse_csr but got self, src: ",
      self.layout(),
      src.layout());
  if (!_is_same_size_as_sparse_csr(self, src)) {
    get_sparse_csr_impl(self)->resize_as_sparse_csr_tensor_(src);
  }
  return self;
}

} // namespace native
} // namespace at
