#include "alpha.h"

#include <iomanip>
#include <map>

#include "logger.h"

namespace openalpha {

static std::map<std::string, int> kUsedAlphaFileNames;

Alpha* Alpha::Initialize(const std::string& name, ParamMap&& params) {
  name_ = name;
  params_ = std::move(params);
  num_dates_ = dr_.GetData("date")->num_rows();
  num_symbols_ = dr_.GetData("symbol")->num_rows();
  auto n = num_dates_ * num_symbols_;
  auto raw_alpha = new double[n];  // memory leak
  auto raw_valid = new bool[n];    // memory leak
  for (auto i = 0lu; i < n; ++i) {
    raw_alpha[i] = kNaN;
    raw_valid[i] = false;
  }
  alpha_ = new double*[num_dates_];
  valid_ = new bool*[num_dates_];
  for (auto i = 0lu; i < num_dates_; ++i) {
    alpha_[i] = raw_alpha + i * num_symbols_;
    valid_[i] = raw_valid + i * num_symbols_;
  }

  auto param = GetParam("delay");
  if (param.size()) delay_ = atoi(param.c_str());
  param = GetParam("decay");
  if (param.size()) decay_ = atoi(param.c_str());
  param = GetParam("universe");
  if (param.size()) universe_ = atoi(param.c_str());
  param = GetParam("lookback_days");
  if (param.size()) lookback_days_ = atoi(param.c_str());
  param = GetParam("book_size");
  if (param.size()) book_size_ = atof(param.c_str());
  param = GetParam("max_stock_weight");
  if (param.size()) max_stock_weight_ = atof(param.c_str());
  param = GetParam("neutralization");
  if (param == kNeutralizationByMarket)
    neutralization_ = kNeutralizationByMarket;
  else if (param == kNeutralizationBySector)
    neutralization_ = kNeutralizationBySector;
  else if (param == kNeutralizationByIndustry)
    neutralization_ = kNeutralizationByIndustry;
  else if (param == kNeutralizationBySubIndustry)
    neutralization_ = kNeutralizationBySubIndustry;
  LOG_INFO("Alpha: " << name << "\ndelay=" << delay_ << "\ndecay=" << decay_
                     << "\nuniverse=" << universe_ << "\nlookback_days="
                     << lookback_days_ << "\nbook_size=" << book_size_
                     << "\nmax_stock_weight=" << max_stock_weight_
                     << "\nneutralization=" << neutralization_);

  return this;
}

Alpha* PyAlpha::Initialize(const std::string& name, ParamMap&& params) {
  Alpha::Initialize(name, std::move(params));

  auto path = fs::path(GetParam("alpha"));
  bp::import("sys").attr("path").attr("insert")(0, path.parent_path().string());
  auto fn = path.filename().string();
  if (!fs::exists(path)) {
    LOG_FATAL("Alpha: can't open file '" + path.string() + "': No such file");
  }

  auto module_name =
      fn.substr(0, fn.length() - path.extension().string().length());
  auto n = kUsedAlphaFileNames[fn]++;
  if (n > 0) {
    module_name += "__" + std::to_string(n);
    auto fn2 = module_name + ".py";
    auto path2 = "'" + (path.parent_path() / fn2).string() + "'";
    if (system(("[ ! -s " + path2 + " ] && ln -s '" + fn + "' " + path2)
                   .c_str())) {
      // only for avoiding compile warning
    }
    LOG_INFO("Alpha: link '" + path.string() + "' to '" + fn2 + "'");
  }

  try {
    auto module = bp::import(module_name.c_str());
    auto dr = bp::object(kOpenAlpha.attr("dr"));
    PyModule_AddStringConstant(module.ptr(), "name", name_.c_str());
    bp::dict py_params;
    for (auto& pair : this->params()) py_params[pair.first] = pair.second;
    PyModule_AddObject(module.ptr(), "dr", dr.ptr());
    auto ptr = py_params.ptr();
    Py_INCREF(ptr);  // memory leak
    PyModule_AddObject(module.ptr(), "params", ptr);
    auto np_valid = np::from_data(
        &valid_[0][0], np::dtype::get_builtin<bool>(),
        bp::make_tuple(num_dates(), num_symbols()),
        bp::make_tuple(num_symbols() * sizeof(bool), sizeof(bool)),
        bp::object());
    ptr = np_valid.ptr();
    Py_INCREF(ptr);  // memory leak
    PyModule_AddObject(module.ptr(), "valid", ptr);
    PyModule_AddIntConstant(module.ptr(), "delay", delay_);
    PyModule_AddIntConstant(module.ptr(), "decay", decay_);
    generate_func_ = GetCallable(module, "generate");
    if (!generate_func_) {
      LOG_FATAL("Alpha: 'generate' function not defined in '" + path.string() +
                "'");
    }
    LOG_INFO("Alpha: '" << path.string() << "' loaded");
  } catch (const bp::error_already_set& err) {
    PrintPyError("Alpha: failed to load '" + path.string() + "': ", true, true);
  }
  bp::import("sys").attr("path").attr("reverse");
  bp::import("sys").attr("path").attr("pop");
  bp::import("sys").attr("path").attr("reverse");

  int_array_.resize(num_symbols_);
  double_array_.resize(num_symbols_);
  pos_.resize(num_symbols_, kNaN);
  ret_.resize(num_dates_, kNaN);
  turnover_.resize(num_dates_, kNaN);

  return this;
}

void Alpha::UpdateValid(int di) {
  // https://github.com/apache/arrow/blob/master/cpp/examples/arrow/row-wise-conversion-example.cc
  auto values =
      std::static_pointer_cast<arrow::DoubleArray>(
          dr_.GetData("adv60_t")->column(di - delay_)->data()->chunk(0))
          ->raw_values();
  for (auto i = 0u; i < int_array_.size(); ++i) int_array_[i] = i;
  std::sort(int_array_.begin(), int_array_.end(),
            [&values](auto i, auto j) { return values[i] < values[j]; });
  auto valid = valid_[di];
  auto n = 0;
  for (auto it = int_array_.rbegin(); it != int_array_.rend() && n < universe_;
       ++it, ++n) {
    valid[*it] = true;
  }
}

void Alpha::Calculate(int di) {
  auto groups = neutralization_ != kNeutralizationByMarket
                    ? dr_.Values<int64_t>(neutralization_ + "_t", di - delay_)
                    : nullptr;
  auto alpha = alpha_[di];
  auto valid = valid_[di];
  std::map<int64_t, std::vector<int>> grouped;
  auto pos_1 = double_array_;
  for (auto ii = 0u; ii < num_symbols_; ++ii) {
    pos_1[ii] = pos_[ii];
    pos_[ii] = kNaN;
    auto v = alpha[ii];
    if (!valid[ii]) {
      if (!std::isnan(v)) alpha[ii] = kNaN;
      continue;
    }
    if (std::isnan(alpha[ii])) continue;
    if (decay_ > 1) {
      auto nsum = decay_;
      auto sum = decay_ * v;
      for (auto j = 1; j < decay_; ++j) {
        auto di2 = di - j;
        if (di2 < 0) continue;
        auto v2 = alpha_[di2][ii];
        if (std::isnan(v2)) continue;
        auto n = decay_ - j;
        nsum += n;
        sum += v2 * n;
      }
      v = sum / nsum;
    }
    pos_[ii] = v;
    auto ig = groups ? 0 : groups[ii];
    if (ig > 0) grouped[ig].push_back(ii);
  }

  double sum;
  auto max_try = 10;
  for (auto itry = 0; itry <= max_try; ++itry) {
    sum = 0;
    for (auto& pair : grouped) {
      if (pair.second.size() == 1) {
        auto ii = pair.second[0];
        pos_[ii] = kNaN;
        continue;
      }
      auto sum2 = 0.;
      for (auto ii : pair.second) sum2 += pos_[ii];
      auto avg = sum2 / pair.second.size();
      for (auto ii : pair.second) {
        pos_[ii] -= avg;
        sum += std::abs(pos_[ii]);
      }
    }
    if (sum == 0) return;
    if (max_stock_weight_ <= 0 || itry == max_try) break;
    auto max_value = max_stock_weight_ * sum;
    auto threshold = max_value * 1.01;
    auto breach = false;
    for (auto v : pos_) {
      if (std::isnan(v)) continue;
      if (std::abs(v) > threshold) {
        breach = true;
        break;
      }
    }
    if (!breach) break;
    for (auto& v : pos_) {
      if (std::isnan(v)) continue;
      if (std::abs(v) > max_value) v = max_value * (v > 0 ? 1 : -1);
    }
  }

  auto pnl = 0.;
  auto close0 = dr_.Values<double>("close_t", di);
  auto close_1 = dr_.Values<double>("close_t", di - 1);
  for (auto ii = 0u; ii < num_symbols_; ++ii) {
    auto& v = pos_[ii];
    if (std::isnan(v)) continue;
    v = std::round(v / sum * book_size_);
    auto ret = close0[ii] / close_1[ii] - 1;
    pnl += v * ret;
  }
  ret_[di] = pnl / book_size_;

  auto turnover = 0.;
  for (auto ii = 0u; ii < num_symbols_; ++ii) {
    auto v = pos_[ii];
    if (std::isnan(v)) v = 0;
    auto v_1 = pos_1[ii];
    if (std::isnan(v_1)) v_1 = 0;
    turnover += v - v_1;
  }
  turnover_[di] = turnover / book_size_ / 2;
}

void PyAlpha::Generate(int di, double* alpha) {
  auto py_alpha =
      np::from_data(alpha, np::dtype::get_builtin<decltype(alpha[0])>(),
                    bp::make_tuple(num_symbols()),
                    bp::make_tuple(sizeof(alpha[0])), bp::object());
  try {
    generate_func_(di, py_alpha);
  } catch (const bp::error_already_set& err) {
    PrintPyError("Alpha: failed to run '" + GetParam("alpha") + "': ", true,
                 true);
  }
}

void AlphaRegistry::Run() {
  auto num_dates = dr_.GetData("date")->num_rows();
  for (auto i = 1; i < num_dates; ++i) {
    for (auto& pair : alphas_) {
      auto alpha = pair.second;
      if (i < alpha->lookback_days_ + alpha->delay_) continue;
      alpha->UpdateValid(i);
      alpha->Generate(i, alpha->alpha_[i]);
      alpha->Calculate(i);
    }
  }
  auto date = dr_.Values<int64_t>("date", 0);
  for (auto& pair : alphas_) {
    auto alpha = pair.second;
    auto path = kStorePath / alpha->name();
    if (!fs::exists(path)) fs::create_directory(path);
    std::ofstream os((path / "perf.csv").string().c_str());
    auto ret = alpha->ret_;
    auto turnover = alpha->turnover_;
    os << "date,return,turnover\n";
    for (auto di = 0; di < num_dates; ++di) {
      auto v = ret[di];
      if (std::isnan(v)) continue;
      os << std::setprecision(15) << date[di] << ',' << v << ',' << turnover[di]
         << '\n';
    }
  }
}

}  // namespace openalpha