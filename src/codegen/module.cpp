#include "taco/codegen/module.h"

#include <iostream>
#include <fstream>
#include <dlfcn.h>
#include <unistd.h>

#include "taco/error.h"
#include "taco/util/strings.h"
#include "taco/util/env.h"
#include "codegen/codegen_c.h"
#include "codegen/codegen_cuda.h"
#include "taco/cuda.h"
#include "codegen/codegen_llvm.h"
#include "codegen/llvm_headers.h"

using namespace std;

namespace taco {
namespace ir {

void Module::setJITTmpdir() {
  tmpdir = util::getTmpdir();
}

void Module::setJITLibname() {
  string chars = "abcdefghijkmnpqrstuvwxyz0123456789";
  libname.resize(12);
  for (int i=0; i<12; i++)
    libname[i] = chars[rand() % chars.length()];
}

void Module::addFunction(Stmt func) {
  
  for(auto i = funcs.begin(); i < funcs.end(); i++){
    if((*i).as<Function>()->name == func.as<Function>()->name){
      funcs.erase(i); 
    }
  }
  funcs.push_back(func); 
}

void Module::compileToSource(string path, string prefix) {
  std::string sourceSuffix;
  if (!moduleFromUserSource) {
  
    // create a codegen instance and add all the funcs
    bool didGenRuntime = false;
    
    header.str("");
    source.str("");
    header.clear();
    source.clear();
    if (target.arch == Target::C99) {
      std::shared_ptr<CodeGen> sourcegen =
          CodeGen::init_default(source, CodeGen::C99Implementation);
      CodeGen_C headergen(header, CodeGen_C::OutputKind::C99Header);
    
    
      for (auto func: funcs) {
        sourcegen->compile(func, !didGenRuntime);
        headergen.compile(func, !didGenRuntime);
        didGenRuntime = true;
      }
      
      sourceSuffix = ".c";
      
      ofstream source_file;
      source_file.open(path+prefix+sourceSuffix);
      source_file << source.str();
      source_file.close();
      
    } else {
      llvm::LLVMContext context;
      CodeGen_LLVM llvm_codegen(target, context);
      CodeGen_C headergen(header, CodeGen_C::OutputKind::C99Header);
      
      for (auto func: funcs) {
        llvm_codegen.compile(func, !didGenRuntime);
        headergen.compile(func, !didGenRuntime);
        didGenRuntime = true;
      }
      sourceSuffix = ".bc";
      llvm_codegen.optimizeModule();
      llvm_codegen.writeToFile(path+prefix+sourceSuffix);
    }
    
  }

  ofstream source_file;
  string file_ending = should_use_CUDA_codegen() ? ".cu" : ".c";
  source_file.open(path+prefix+file_ending);
  source_file << source.str();
  source_file.close();
  
  ofstream header_file;
  header_file.open(path+prefix+".h");
  header_file << header.str();
  header_file.close();
}

void Module::compileToStaticLibrary(string path, string prefix) {
  taco_tassert(false) << "Compiling to a static library is not supported";
}
  
namespace {

void writeShims(vector<Stmt> funcs, string path, string prefix) {
  stringstream shims;
  for (auto func: funcs) {
    if (should_use_CUDA_codegen()) {
      CodeGen_CUDA::generateShim(func, shims);
    }
    else {
      CodeGen_C::generateShim(func, shims);
    }
  }
  
  ofstream shims_file;
  string file_ending;
  if (should_use_CUDA_codegen()) {
    file_ending = ".cpp";
  }
  else {
    file_ending = ".c";
  }
  shims_file.open(path+prefix+"_shims" + file_ending);
  shims_file << "#include \"" << path << prefix << ".h\"\n";
  shims_file << shims.str();
  shims_file.close();
}

} // anonymous namespace

string Module::compile() {
  string prefix = tmpdir+libname;
  string fullpath = prefix + ".so";
  
  string cc;
  string cflags;
  string file_ending;
  string shims_file_ending;
  if (should_use_CUDA_codegen()) {
    cc = "nvcc";
    cflags = util::getFromEnv("TACO_NVCCFLAGS",
    get_default_CUDA_compiler_flags());
    file_ending = ".cu";
    shims_file_ending = ".cpp";
  }
  else {
    cc = util::getFromEnv(target.compiler_env, target.compiler);
    cflags = util::getFromEnv("TACO_CFLAGS",
    "-O3 -ffast-math -std=c99") + " -shared -fPIC";
    file_ending = ".c";
    shims_file_ending = ".c";
  }
  
  string cmd = cc + " " + cflags + " " +
    prefix + (target.arch == Target::X86 ? ".s " : ".c ") +
    (target.arch == Target::C99 ? prefix + "_shims.c " : "") +
    "-o " + prefix + ".so";
    
  // open the output file & write out the source
  compileToSource(tmpdir, libname);
  
  // write out the shims
  if (target.arch == Target::C99) {
    writeShims(funcs, tmpdir, libname);
  }
  
  if (target.arch == Target::X86) {
    // use llc to compile the .bc file
    string llcCommand = util::getFromEnv("TACO_LLC", "llc") +
      " " + prefix + ".bc";
    int err = system(llcCommand.data());
    taco_uassert(err == 0) << "Compilation command failed:\n" << cmd
      << "\nreturned " << err;
  }
  
  // now compile it
  int err = system(cmd.data());
  taco_uassert(err == 0) << "Compilation command failed:\n" << cmd
    << "\nreturned " << err;

  // use dlsym() to open the compiled library
  lib_handle = dlopen(fullpath.data(), RTLD_NOW | RTLD_LOCAL);

  return fullpath;
}

void Module::setSource(string source) {
  this->source << source;
  moduleFromUserSource = true;
}

string Module::getSource() {
  return source.str();
}

void* Module::getFuncPtr(std::string name) {
  return dlsym(lib_handle, name.data());
}

int Module::callFuncPackedRaw(std::string name, void** args) {
  typedef int (*fnptr_t)(void**);
  static_assert(sizeof(void*) == sizeof(fnptr_t),
    "Unable to cast dlsym() returned void pointer to function pointer");
  void* v_func_ptr = getFuncPtr(name);
  fnptr_t func_ptr;
  *reinterpret_cast<void**>(&func_ptr) = v_func_ptr;
  return func_ptr(args);
}

} // namespace ir
} // namespace taco
