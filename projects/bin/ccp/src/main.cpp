#include <cc/pipeline.hpp>

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

namespace {
struct args {
  std::string front;
  std::string arch;
  std::string input;
  std::string output = "a.out";
};

bool parse_args(int argc, char** argv, args& a) {
  for (int i = 1; i < argc; ++i) {
    std::string_view v = argv[i];
    if (v.starts_with("--front=")) {
      a.front = std::string{v.substr(8)};
    } else if (v.starts_with("--arch=")) {
      a.arch = std::string{v.substr(7)};
    } else if (v == "-o") {
      if (++i < argc) a.output = argv[i];
    } else if (v.starts_with("-o")) {
      a.output = std::string{v.substr(2)};
    } else if (v == "--help" || v == "-h") {
      std::cout << "usage: ccp --front=<lang> --arch=<arch> <input> -o <output>\n";
      return false;
    } else {
      a.input = std::string{v};
    }
  }
  return !a.front.empty() && !a.arch.empty() && !a.input.empty();
}
}  // namespace

int main(int argc, char** argv) {
  args a;
  if (!parse_args(argc, argv, a)) {
    std::cerr << "usage: ccp --front=<lang> --arch=<arch> <input> -o <output>\n";
    return argc <= 1 ? 0 : 1;
  }

  std::ifstream in{a.input};
  if (!in) {
    std::cerr << "ccp: cannot open '" << a.input << "'\n";
    return 1;
  }
  std::string source((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());

  auto built = cc::pipeit::pipeline_builder{}
                   .front(a.front)
                   .back(a.arch)
                   .build();
  if (!built) {
    std::cerr << "ccp: " << built.error() << "\n";
    return 1;
  }
  auto pipe = std::move(*built);
  if (!pipe.run(source, a.output)) {
    std::cerr << "ccp: pipeline failed\n";
    return 1;
  }
  std::cout << "ccp: " << a.input << " -> " << a.output << " (front=" << a.front
            << ", arch=" << a.arch << ")\n";
  return 0;
}
