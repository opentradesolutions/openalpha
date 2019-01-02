#include "data.h"

#include <boost/type_index.hpp>

#include "python.h"

namespace openalpha {

DataRegistry::Array DataRegistry::GetData(const std::string& name) {
  auto out = array_map_[name];
  if (out) return out;
  try {
    auto obj = bp::import("pyarrow.parquet")
                   .attr("read_table")((kCachePath / (name + ".par")).string());
    arrow::py::unwrap_table(obj.ptr(), &out);
    array_map_[name] = out;
    LOG_INFO("DataRegistry: " << name << " loaded");
  } catch (const bp::error_already_set& err) {
    PrintPyError("DataRegistry: failed to load '" + name + "': ", true);
  }
  return out;
}

bool DataRegistry::Has(const std::string& name) {
  auto path = kCachePath / (name + ".par");
  return fs::exists(path);
}

bp::object DataRegistry::GetPy(const std::string& name) {
  auto out = py_array_map_[name];
  if (out) return out;
  auto array = GetData(name);
  auto ptr = arrow::py::wrap_table(array);
  out = bp::object(bp::handle<>(bp::borrowed(ptr)))
            .attr("to_pandas")()
            .attr("values");
  assert(Py_REFCNT(ptr) == 1);
  py_array_map_[name] = out;
  return out;
}

void DataRegistry::Initialize() {
  GetPy("symbol");
  GetPy("date");
}

}  // namespace openalpha