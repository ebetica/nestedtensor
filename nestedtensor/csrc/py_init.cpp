#include <nestedtensor/csrc/creation.h>
#include <nestedtensor/csrc/nested_tensor_impl.h>
#include <nestedtensor/csrc/python_functions.h>
#include <nestedtensor/csrc/utils/nested_node_functions.h>
#include <nestedtensor/csrc/utils/python_nested_node.h>
#include <torch/csrc/Size.h>
#include <torch/csrc/autograd/python_variable_indexing.h>
#include <torch/extension.h>
#include <chrono>

// NOTE: A NestedTensor without any constituents, i.e.
// nested_tensor([]) is of dimension 1 because
// tensor([]) is of dimension 1, but it is also
// of nested_dim 1, since there are no constituents
// and thus we choose that to imply that the constituents
// tensor dimension is 0.
// If depth is 0, it means that the current structure
// is already a leaf, i.e. has no children.

namespace py = pybind11;

using namespace torch::nested_tensor;
using namespace at;

Tensor get_item(Tensor tensor, py::none key) {
  std::vector<TensorNode> result_nodes;
  result_nodes.push_back(get_nested_tensor_structure(tensor));
  return wrap_tensor_node(TensorNode(std::move(result_nodes)));
}

at::Tensor get_item(Tensor tensor, int64_t key_) {
  std::vector<at::Tensor> unbound = unbind(tensor, 0);
  int64_t key = at::maybe_wrap_dim(key_, unbound.size());
  return unbound[key];
}

#if (PYBIND11_VERSION_MAJOR >= 2 && PYBIND11_VERSION_MINOR >= 3)
at::Tensor get_item(Tensor tensor, py::slice slice) {
  size_t start, stop, step, slicelength;
  if (!slice.compute(tensor.size(0), &start, &stop, &step, &slicelength))
    throw py::error_already_set();
  return at::slice(tensor, 0, start, stop, step);
}

at::Tensor get_item(Tensor tensor, std::vector<py::object> key) {
  if (key.size() == 0) {
    return tensor;
  }
  c10::optional<at::Tensor> first;
  if (!is_nested_tensor_impl(tensor)) {
    auto wrapped_key = py::tuple(py::cast(key));
    auto wrapped_tensor = THPVariable_Wrap(tensor);
    auto wrapped_result = torch::autograd::THPVariable_getitem(wrapped_tensor, wrapped_key.ptr());
    auto result = THPVariable_Unpack(wrapped_result);
    Py_DECREF(wrapped_tensor);
    Py_DECREF(wrapped_result);
    return result;
  }
  std::vector<py::object> rest;
  for (size_t i = 1; i < key.size(); i++) {
    rest.push_back(key[i]);
  }
  if (is_nested_tensor_impl(tensor) && py::isinstance<py::none>(key[0])) {
    first = get_item(tensor, py::cast<py::none>(key[0]));
  }
  if (is_nested_tensor_impl(tensor) && py::isinstance<py::int_>(key[0])) {
    first = get_item(tensor, py::cast<int64_t>(key[0]));
  }
  if (is_nested_tensor_impl(tensor) && py::isinstance<py::slice>(key[0])) {
    first = get_item(tensor, py::cast<py::slice>(key[0]));
  }
  TORCH_CHECK(
      first,
      "First entry of tuple doesn't have accepted type. ",
      py::str(key[0]));
  if (!is_nested_tensor_impl(*first)) {
    return get_item(*first, rest);
  }
  std::vector<at::Tensor> result;
  for (auto t : (*first).unbind()) {
    result.push_back(get_item(t, rest));
  }
  int64_t nested_dim = get_nested_tensor_impl(*first)->nested_dim();
  std::vector<TensorNode> result_nodes;
  if (nested_dim == 1) {
    for (auto t: result) {
      result_nodes.push_back(TensorNode(std::move(t)));
    }
  } else {
    for (auto t: result) {
      result_nodes.push_back(get_nested_tensor_structure(t));
    }
  }
  return wrap_tensor_node(TensorNode(std::move(result_nodes)));
}

at::Tensor get_item(Tensor tensor, py::tuple key) {
  std::vector<py::object> entries;
  for (size_t i = 0; i < key.size(); i++) {
    entries.push_back(key[i]);
  }
  auto result = get_item(tensor, entries);
  return result;
}
#endif

py::object _nested_helper(c10::optional<int64_t> index, SizeNode&& size_node) {
  auto fn = [](auto& self, const SizeNode& s, int64_t dim) -> py::object {
    if (dim == 0) {
      return py::cast(s.degree());
    }
    // List of Tensors
    if (s.height() == 1) {
      std::vector<int64_t> result;
      for (const auto& child : s.unbind()) {
        result.push_back(child.payload().get(dim - 1));
      }
      return py::tuple(py::cast(result));
    }
    std::vector<py::object> result;
    for (const auto& child : s.unbind()) {
      result.emplace_back(self(self, child, dim - 1));
    }
    return py::tuple(py::cast(result));
  };
  return fn(fn, size_node, *index);
}


namespace torch {
namespace nested_tensor {
namespace {

inline std::vector<std::string> split_str(
    std::string s,
    std::string delimiter) {
  std::vector<std::string> result;
  size_t pos = 0;
  std::string token;
  while ((pos = s.find(delimiter)) != std::string::npos) {
    token = s.substr(0, pos);
    result.push_back(token);
    s.erase(0, pos + delimiter.length());
  }
  result.push_back(s);
  return result;
}

static auto registry =
    torch::RegisterOperators()
        .op("nestedtensor::is_nested_tensor_impl",
            [](Tensor tensor) { return is_nested_tensor_impl(tensor); })
        .op("nestedtensor::nested_dim",
            [](Tensor tensor) {
              return get_nested_tensor_impl(tensor)->nested_dim();
            })
        .op("nestedtensor::stack",
            [](std::vector<Tensor> tensors, int64_t dim) {
              return at::stack(TensorList(tensors), dim);
            })
        .op("nestedtensor::cat",
            [](std::vector<Tensor> tensors, int64_t dim) {
              return at::cat(TensorList(tensors), dim);
            })
        .op("nestedtensor::to_nested_tensor",
            [](Tensor tensor, c10::optional<int64_t> dim) {
              return get_nested_tensor_impl(tensor)->to_nested_tensor(dim);
            })
        .op("nestedtensor::grad",
            [](Tensor tensor) {
              return get_nested_tensor_impl(tensor)->grad();
            })
        .op("nestedtensor::requires_grad",
            [](Tensor tensor) {
              return get_nested_tensor_impl(tensor)->requires_grad();
            })
        .op("nestedtensor::requires_grad_",
            [](Tensor tensor, bool requires_grad) {
              auto nt = get_nested_tensor_impl(tensor);
              return nt->requires_grad_(requires_grad);
            })
        .op("nestedtensor::backward",
            [](Tensor tensor,
               Tensor gradient,
               bool retain_graph,
               bool create_graph) {
              auto nt = get_nested_tensor_impl(tensor);
              nt->backward(gradient, retain_graph, create_graph);
            })
        .op("nestedtensor::sizes",
            [](Tensor tensor) {
              return get_nested_tensor_impl(tensor)->opt_sizes();
            })
        .op("nestedtensor::len",
            [](Tensor self) {
              return (int64_t)(get_nested_tensor_structure(self).degree());
            })
        .op("nestedtensor::to_tensor",
            [](Tensor tensor, c10::optional<int64_t> dim) {
              return NestedTensor_to_tensor(tensor, dim);
            })
        .op("nestedtensor::str", [](Tensor tensor) {
          auto node = get_nested_tensor_structure(tensor);
          return NestedNode___str__(
              node,
              "nested_tensor",
              [](c10::IValue payload, const std::string& tabs) {
                std::stringstream ss;
                ss << payload;
                std::vector<std::string> tokens = split_str(ss.str(), "\n");
                size_t data_lines = tokens.size() - 1;
                std::string result;
                size_t max_lines = 3;
                size_t i = 0;
                for (; i < std::min(max_lines, data_lines); i++) {
                  result += "\n";
                  result += tabs + tokens[i];
                }
                if (2 * max_lines < data_lines) {
                  i = std::max(i, data_lines - max_lines);
                  result += "\n" + tabs + "...";
                }
                for (; i < data_lines; i++) {
                  result += "\n";
                  result += tabs + tokens[i];
                }
                result += "\n" + tabs + tokens[data_lines];
                return result;
              });
        });
} // namespace
} // namespace nested_tensor
} // namespace torch

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  register_python_nested_node(m);
  // NOTE: Never forget about pybind return value policies
  // since you can expect transparent changes to the constiuents
  // via unbind.

  m.def("nested_tensor_impl", &torch::nested_tensor::nested_tensor_impl);

  // Need to overwrite because
  // https://github.com/pytorch/pytorch/blob/09660896c0dd2bec888857300a7be9edb52dd05d/aten/src/ATen/TensorIndexing.h#L480
  // requires sizes() for non Tensor-shape compliant NestedTensors
  // and can't be overwritten since it's not a native function.
  // TODO: Advanced indexing
  // TODO: Tensor-wise select
  // TODO: Tuple support
  m.def("get_item", [](Tensor tensor, py::none key) {
    return get_item(tensor, key);
  });
  m.def("get_item", [](Tensor tensor, int64_t key) {
    return get_item(tensor, key);
  });
#if (PYBIND11_VERSION_MAJOR >= 2 && PYBIND11_VERSION_MINOR >= 3)
  m.def("get_item", [](Tensor tensor, py::slice key) {
    return get_item(tensor, key);
  });
  m.def("get_item", [](Tensor tensor, py::tuple key) {
    return get_item(tensor, key);
  });
#endif

  m.def("nested_size", [](Tensor self, c10::optional<int64_t> index_) {
    auto nt = get_nested_tensor_impl(self);
    if (!index_) {
      return py::cast(THPPythonNode(
          map(
              [](c10::List<int64_t> e) {
                std::vector<int64_t> e_vec = e.vec();
                return py::reinterpret_steal<py::object>(
                    THPSize_NewFromSizes(e_vec.size(), e_vec.data()));
              },
              nt->nested_size()),
          "NestedSize"));
    }
    int64_t index = at::maybe_wrap_dim((*index_), nt->dim());
    SizeNode size_node = nt->nested_size();
    return _nested_helper(index, std::move(size_node));
  });

  m.def("nested_stride", [](Tensor self, c10::optional<int64_t> index_) {
    auto nt = get_nested_tensor_impl(self);
    if (!index_) {
      return py::cast(THPPythonNode(
          map([](c10::List<int64_t> e)
                  -> py::object { return py::tuple(py::cast(e.vec())); },
              nt->nested_stride()),
          "NestedStride"));
    }
    int64_t index = at::maybe_wrap_dim((*index_), nt->dim());
    SizeNode size_node = nt->nested_stride();
    return _nested_helper(index, std::move(size_node));
  });

  add_functions(m);
}
