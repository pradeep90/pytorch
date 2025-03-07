#include <pybind11/pybind11.h>
#include <torch/csrc/jit/backends/backend.h>
#include <torch/csrc/jit/backends/backend_preprocess.h>
#include <torch/csrc/jit/python/pybind_utils.h>

namespace py = pybind11;

// Converts model to Android NNAPI backend and serializes it for mobile
// Returns a dictionary string with one entry:
// Key: "NnapiModule"
// Value: Android NNAPI module serialized for mobile, as a string
//
// method_compile_spec should contain a Tensor or
// Tensor List which bundles several input parameters:
// shape, dtype, quantization, and dimorder (NHWC/NCHW)
// For input shapes, use 0 for run/load time flexible input
//
// The compile_spec should include the format:
// {"forward": {"inputs": at::Tensor}}
// OR {"forward": {"inputs": c10::List<at::Tensor>}}
// Example input Tensor:
// torch.tensor([[1.0, -1.0, 2.0, -2.0]]).unsqueeze(-1).unsqueeze(-1)
//
// In the future, preprocess will accept a dedicated object
c10::IValue preprocess(
    const torch::jit::Module& mod,
    const c10::Dict<c10::IValue, c10::IValue>& method_compile_spec,
    const torch::jit::BackendDebugHandleGenerator& generate_debug_handles) {
  // Import the python function for converting modules to Android NNAPI backend
  py::gil_scoped_acquire gil;
  py::object pyModule = py::module_::import("torch.backends._nnapi.prepare");
  py::object pyMethod = pyModule.attr("convert_model_to_nnapi");

  // Wrap the c module in a RecursiveScriptModule
  auto out =
      py::module::import("torch.jit._recursive").attr("wrap_cpp_module")(mod);
  out.attr("eval")();

  // Test that method_compile_spec contains the necessary keys and
  // Tensor/TensorList input
  c10::IValue inp;
  std::string error = "";
  if (!method_compile_spec.contains("forward")) {
    error = R"(method_compile_spec does not contain the "forward" key.)";
  } else {
    auto innerDict = method_compile_spec.at("forward");
    if (!innerDict.isGenericDict() ||
        !innerDict.toGenericDict().contains("inputs")) {
      error =
          R"(method_compile_spec does not contain a dictionary with an "inputs" key, under it's "forward" key.)";
    } else {
      inp = innerDict.toGenericDict().at("inputs");
      if (!inp.isTensor() && !inp.isTensorList()) {
        error =
            R"(method_compile_spec does not contain either a Tensor or TensorList, under it's "inputs" key.)";
      }
    }
  }
  if (error.size() != 0) {
    throw std::runtime_error(
        error +
        "\nmethod_compile_spec should contain a Tensor or Tensor List which bundles input parameters:"
        " shape, dtype, quantization, and dimorder."
        "\nFor input shapes, use 0 for run/load time flexible input."
        "\nmethod_compile_spec must use the following format:"
        "\n{\"forward\": {\"inputs\": at::Tensor}} OR {\"forward\": {\"inputs\": c10::List<at::Tensor>}}");
  }

  // Convert input to a Tensor or a python list of Tensors
  py::object nnapi_pyModel;

  if (inp.isTensor()) {
    nnapi_pyModel = pyMethod(out, inp.toTensor());
  } else {
    py::list pyInp;
    for (at::Tensor inpElem : inp.toTensorList()) {
      pyInp.append(inpElem);
    }
    nnapi_pyModel = pyMethod(out, pyInp);
  }

  // Cast the returned py object and save it for mobile
  std::stringstream ss;
  auto nnapi_model = py::cast<torch::jit::Module>(nnapi_pyModel.attr("_c"));
  nnapi_model._save_for_mobile(ss);

  c10::Dict<c10::IValue, c10::IValue> dict(
      c10::StringType::get(), c10::StringType::get());
  dict.insert("NnapiModule", ss.str());
  return dict;
}

constexpr auto backend_name = "nnapi";
static auto pre_reg =
    torch::jit::backend_preprocess_register(backend_name, preprocess);
