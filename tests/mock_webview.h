#pragma once
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>
namespace webview {
template <class T> struct res { T v; bool ok() const { return true; } T value() const { return v; } };
struct webview {
  std::map<std::string, std::function<std::string(std::string)>> b;
  std::mutex m; std::vector<std::string> evals;
  webview(bool, void *) {}
  res<void *> window() { return {nullptr}; }
  res<void *> widget() { return {nullptr}; }
  res<void *> browser_controller() { return {nullptr}; }
  void set_title(const std::string &) {}
  void bind(const std::string &n, std::function<std::string(std::string)> f) { b[n] = f; }
  void dispatch(std::function<void()> f) { f(); }
  void eval(const std::string &s) { std::lock_guard<std::mutex> l(m); evals.push_back(s); }
  void set_html(const std::string &) {}
  void run() {}
  std::string call(const std::string &n, const std::string &args) { return b.at(n)(args); }
  std::string last() { std::lock_guard<std::mutex> l(m); return evals.empty() ? "" : evals.back(); }
};
}
