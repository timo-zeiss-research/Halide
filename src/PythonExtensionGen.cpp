#include <iostream>
#include <string>

#include "Module.h"
#include "PythonExtensionGen.h"
#include "Util.h"

using std::ostream;
using std::ostringstream;
using std::string;

namespace Halide {
namespace Internal {

static string sanitize_name(const string &name) {
    ostringstream oss;
    for (size_t i = 0; i < name.size(); i++) {
        if (name[i] == '.' || name[i] == '_') {
            oss << '_';
        } else if (!isalnum(name[i])) {
            oss << "_" << (int)name[i];
        } else {
          oss << name[i];
        }
    }
    return oss.str();
}

static const string remove_namespaces(const string &name) {
    size_t i = name.find_last_of(":");
    if (i == string::npos) {
        return name;
    } else {
        return name.substr(i + 1);
    }
}

static bool can_convert(const LoweredArgument* arg) {
  if (arg->type.is_handle() || arg->type.is_vector())
      return false;
  if (arg->type.is_float() && arg->type.bits() != 32 && arg->type.bits() != 64)
      return false;
  return true;
}

std::pair<string, string> print_type(const LoweredArgument* arg) {
    // Excluded by can_convert() above:
    assert(!arg->type.is_handle());
    assert(!arg->type.is_vector());

    if (arg->is_buffer()) {
        return std::make_pair("O", "PyObject*");
    } else if (arg->type.is_float() && arg->type.bits() == 32) {
        return std::make_pair("f", "float");
    } else if (arg->type.is_float() && arg->type.bits() == 64) {
        return std::make_pair("d", "double");
    } else if (arg->type.is_bool()) {
        // "b" expects an unsigned char, so we assume that bool == uint8.
        return std::make_pair("b", "bool");
    } else if (arg->type.is_int()) {
        return std::make_pair("i", "int");
    } else if (arg->type.is_uint()) {
        return std::make_pair("I", "unsigned int");
    } else {
        assert(0);
    }
}

void PythonExtensionGen::convert_buffer(string name, const LoweredArgument* arg) {
  assert(arg->is_buffer());
  dest << "    Py_buffer py_buffer_" << name << ";\n";
  dest << "    halide_buffer_t buffer_" << name << ";\n";
  dest << "    if (_get_py_buffer(py_" << name << ", &py_buffer_" << name << ", ";
  dest << (int)arg->dimensions << ", ";
  dest << (arg->is_output() ? "PyBUF_WRITABLE" : "0") << ", ";
  dest << "\"" << name << "\"";
  dest << ") < 0) {\n";
  dest << "        return NULL;\n";
  dest << "    }\n";
  // TODO: Do we always know the number of dimensions at compile time? If so, we
  // could declare an array of constant size, and merge _get_py_buffer and
  // _convert_py_buffer_to_halide.
  dest << "    halide_dimension_t dimensions_" << name << "[py_buffer_" << name << ".ndim];\n";
  dest << "    if (!_convert_py_buffer_to_halide(&py_buffer_" << name
       << ", dimensions_" << name << ", &buffer_" << name << ", \"" << name << "\")) {\n";
  dest << "        return NULL;\n";
  dest << "    }\n";
}

PythonExtensionGen::PythonExtensionGen(std::ostream &dest, const std::string &header_name, Target target)
    : dest(dest), header_name(header_name), target(target) {
}

void PythonExtensionGen::compile(const Module &module) {
    dest << "#include \"" << header_name << "\"\n";
    dest << "#include \"Python.h\"\n";
    dest << "#include \"HalideRuntime.h\"\n\n";

    dest << "#define MODULE_NAME \"" << module.name() << "\"\n";

    dest << R"INLINE_CODE(
#ifdef __cplusplus
extern "C" {
#endif

static int _get_py_buffer(PyObject* pyobj, Py_buffer* buf, int dimensions, int flags,
                          const char* name) {
    int ret = PyObject_GetBuffer(
      pyobj, buf, PyBUF_FORMAT | PyBUF_STRIDED_RO | PyBUF_C_CONTIGUOUS | flags);
    if (ret < 0) {
      return ret;
    }
    if (dimensions && buf->ndim != dimensions) {
      PyErr_Format(PyExc_ValueError, "Invalid argument %s: Expected %d dimensions, got %d",
                   name, dimensions, buf->ndim);
      return -1;
    }
    return 0;
}

static bool _convert_py_buffer_to_halide(Py_buffer* in,
                                         halide_dimension_t* dim,
                                         halide_buffer_t* out,
                                         const char* name) {
    if (in->shape[0] * in->strides[0] != in->len) {  // length is in bytes, and so is strides
        PyErr_Format(PyExc_ValueError, "Invalid buffer: length %ld, but computed length %ld",
                     in->len, in->shape[0] * in->strides[0]);
        return false;
    }
    /* We ask for PyBUF_C_CONTIGUOUS above, because numpy can't convert to F_CONTIGUOUS.
     * So we're getting a buffer where the last dimension varies the fastest
     * (i.e., has stride=1). Flip the dimensions so the first dimension varies
     * the fastest (has stride=1), which is what Halide needs.
     */
    int i, j;
    for (i = 0, j = in->ndim - 1; i < in->ndim; ++i, --j) {
        dim[i].min = 0;
        dim[i].stride = in->strides[j] / in->itemsize; // strides is in bytes
        dim[i].extent = in->shape[j];
        dim[i].flags = 0;
        if (in->suboffsets && in->suboffsets[i] >= 0) {
            // Halide doesn't support arrays of pointers. But we should never see this
            // anyway, since we specified PyBUF_STRIDED.
            PyErr_Format(PyExc_ValueError, "Invalid buffer: suboffsets not supported");
            return false;
        }
    }
    memset(out, 0, sizeof(*out));
    if (!in->format) {
        out->type.code = halide_type_uint;
        out->type.bits = 8;
    } else {
        /* Convert struct type code. See
         * https://docs.python.org/2/library/struct.html#module-struct */
        char* p = in->format;
        while (strchr("@<>!=", *p)) p++;  // ignore little/bit endian
        if (*p == 'f' && *p <= 'd') {
          // 'f' and 'd' are float and double, respectively.
          out->type.code = halide_type_float;
        } else if (*p >= 'a' && *p <= 'z') {
          // lowercase is signed int.
          out->type.code = halide_type_int;
        } else {
          // uppercase is unsigned int.
          out->type.code = halide_type_uint;
        }
        // 1, 2, 4 and 8 byte types, in blocks of six:
        const char* type_codes = "bB?..|hH...|iIlLf|qQd...";
        const char* type_pos = strchr(type_codes, *p);
        if (type_pos) {
          out->type.bits = 8 << ((type_pos - type_codes) / 6);
        } else {
          // We don't handle 's' and 'p' (char[]) and 'P' (void*)
          PyErr_Format(PyExc_ValueError, "Invalid data type for %s: %s", name, in->format);
          return false;
        }
    }
    out->type.lanes = 1;
    out->dimensions = in->ndim;
    out->dim = dim;
    out->host = (uint8_t*)in->buf;
    return true;
}

)INLINE_CODE";

    for (const auto &f : module.functions()) {
        compile(f);
    }

    dest << "\n";
    dest << "static PyMethodDef _methods[] = {\n";
    for (const auto &f : module.functions()) {
        const string basename = remove_namespaces(f.name);
        dest << "    {\"" << basename << "\", (PyCFunction)_f_" << basename
             << ", METH_VARARGS|METH_KEYWORDS, NULL},\n";
    }
    dest << "    {0, 0, 0, NULL},  // sentinel\n";
    dest << "};\n";

    dest << R"INLINE_CODE(
#if PY_MAJOR_VERSION >= 3
static struct PyModuleDef _moduledef = {
    PyModuleDef_HEAD_INIT,
    .m_name=MODULE_NAME,
    .m_doc=NULL,
    .m_size=-1,
    .m_methods=_methods,
};
PyMODINIT_FUNC PyInit_)INLINE_CODE";
    dest << module.name() << "(void) {";
    dest << R"INLINE_CODE(
    return PyModule_Create(&_moduledef);
}
#else
PyMODINIT_FUNC init)INLINE_CODE";
    dest << module.name() << "(void) {";
    dest << R"INLINE_CODE(
    Py_InitModule3(MODULE_NAME, _methods, NULL);
}
#endif

#ifdef __cplusplus
}
#endif
)INLINE_CODE";
}

void PythonExtensionGen::compile(const LoweredFunc &f) {
    const std::vector<LoweredArgument> &args = f.args;
    const string basename = remove_namespaces(f.name);
    std::vector<string> arg_names(args.size());
    dest << "// " << f.name << "\n";
    dest << "static PyObject* _f_" << basename << "(PyObject* module, PyObject* args, PyObject* kwargs) {\n";
    for (size_t i = 0; i < args.size(); i++) {
        arg_names[i] = sanitize_name(args[i].name);
        if (!can_convert(&args[i])) {
            /* Some arguments can't be converted to Python yet. In those
             * cases, just add a dummy function that always throws an
             * Exception. */
            dest << "    PyErr_Format(PyExc_NotImplementedError, "
                 << "\"Can't convert argument " << args[i].name << " to Python\");\n";
            dest << "    return NULL;\n";
            dest << "}";
            return;
        }
    }
    dest << "    static const char* kwlist[] = {";
    for (size_t i = 0; i < args.size(); i++) {
        if (i > 0) {
            dest << ", ";
        }
        dest << "\"" << arg_names[i] << "\"";
    }
    dest << "};\n";
    for (size_t i = 0; i < args.size(); i++) {
        dest << "    " << print_type(&args[i]).second << " py_" << arg_names[i] << ";\n";
    }
    dest << "    if (!PyArg_ParseTupleAndKeywords(args, kwargs, \"";
    for (size_t i = 0; i < args.size(); i++) {
        dest << print_type(&args[i]).first;
    }
    dest << "\", (char**)kwlist";
    for (size_t i = 0; i < args.size(); i++) {
        dest << ", ";
        dest << "&py_" << arg_names[i];
    }
    dest << ")) {\n";
    dest << "        return NULL;\n";
    dest << "    }\n";
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i].is_buffer()) {
            convert_buffer(arg_names[i], &args[i]);
        } else {
            // Python already converted this.
        }
    }
    dest << "    int result = " << f.name << "(";
    for (size_t i = 0; i < args.size(); i++) {
        if (i > 0) {
            dest << ", ";
        }
        if (args[i].is_buffer()) {
            dest << "&buffer_" << arg_names[i];
        } else {
            dest << "py_" << arg_names[i];
        }
    }
    dest << ");";
    dest << R"INLINE_CODE(
    if (result != 0) {
        /* In the optimal case, we'd be generating an exception declared
         * in python_bindings/src, but since we're self-contained,
         * we don't have access to that API. */
        PyErr_Format(PyExc_ValueError, "Halide error %d", result);
        return NULL;
    }
    Py_INCREF(Py_True);
    return Py_True;
)INLINE_CODE";
    dest << "}\n";
}

}
}