#pragma once

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace june {

// =============================================================================
// Infectiousness Curve Interface
// =============================================================================

class InfectiousnessCurve {
 public:
  virtual ~InfectiousnessCurve() = default;
  virtual double evaluate(double t) const = 0;
  virtual std::unique_ptr<InfectiousnessCurve> clone() const = 0;

  // Precompute antiderivative table F[i] = ∫₀^{i·dt} I(τ)dτ over [0, t_max]
  // using the midpoint rule with n_points intervals. After calling this,
  // integrate() uses the table with linear interpolation.
  void buildIntegralTable(double t_max, int n_points) {
    t_table_max_ = t_max;
    n_table_ = n_points;
    dt_table_ = t_max / static_cast<double>(n_points);
    F_table_.resize(n_points + 1);
    F_table_[0] = 0.0;
    for (int i = 0; i < n_points; ++i) {
      double t_mid = (i + 0.5) * dt_table_;
      F_table_[i + 1] = F_table_[i] + evaluate(t_mid) * dt_table_;
    }
    table_built_ = true;
  }

  // Return ∫_{t0}^{t1} I(τ)dτ. Uses precomputed table with linear
  // interpolation if available; otherwise falls back to midpoint rule.
  double integrate(double t0, double t1) const {
    if (t1 <= t0) return 0.0;
    if (!table_built_) {
      return evaluate(0.5 * (t0 + t1)) * (t1 - t0);
    }
    return interpolateF(std::min(t1, t_table_max_)) -
           interpolateF(std::max(t0, 0.0));
  }

 private:
  double interpolateF(double t) const {
    double idx_f = t / dt_table_;
    int idx = static_cast<int>(idx_f);
    if (idx >= n_table_) return F_table_[n_table_];
    double alpha = idx_f - static_cast<double>(idx);
    return (1.0 - alpha) * F_table_[idx] + alpha * F_table_[idx + 1];
  }

  std::vector<double> F_table_;
  double t_table_max_ = 0.0;
  double dt_table_ = 0.0;
  int n_table_ = 0;
  bool table_built_ = false;
};

// =============================================================================
// Constant Curve - Fixed infectiousness over time
// =============================================================================

class ConstantCurve : public InfectiousnessCurve {
 public:
  explicit ConstantCurve(double value) : value_(value) {}

  double evaluate(double /*t*/) const override { return value_; }

  std::unique_ptr<InfectiousnessCurve> clone() const override {
    return std::make_unique<ConstantCurve>(value_);
  }

 private:
  double value_;
};

// =============================================================================
// Gamma Curve - Standard epidemiological profile
// =============================================================================

class GammaCurve : public InfectiousnessCurve {
 public:
  GammaCurve(double max_infectiousness, double shape, double rate,
             double shift = 0.0)
      : max_inf_(max_infectiousness),
        shape_(shape),
        rate_(rate),
        shift_(shift) {
    // Precompute normalization constants
    double scale = 1.0 / rate_;
    normalization_constant_ = shape_ * std::log(scale) + std::lgamma(shape_);

    // Compute log-PDF at the mode to normalise peak to max_infectiousness.
    // For shape < 1 the PDF is monotonically decreasing (no interior mode);
    // clamp to a small positive value so the normalisation is finite.
    double t_mode = (shape_ > 1.0) ? (shape_ - 1.0) / rate_ : 1e-9;
    log_peak_ = (shape_ - 1.0) * std::log(t_mode) - (t_mode * rate_) -
                normalization_constant_;
  }

  double evaluate(double t) const override {
    double shifted_t = t - shift_;
    if (shifted_t <= 0.0) return 0.0;

    // f(x; k, θ) = (1 / (Γ(k) * θ^k)) * x^(k-1) * e^(-x/θ)
    // log(f) = (k-1)*log(x) - x/theta - log(Gamma(k)) - k*log(theta)
    double log_pdf = (shape_ - 1.0) * std::log(shifted_t) -
                     (shifted_t * rate_) - normalization_constant_;

    // Subtract log_peak_ so evaluate(t_mode) == max_inf_ (dimensionless).
    return max_inf_ * std::exp(log_pdf - log_peak_);
  }

  // Returns the old (pre-fix) peak value: max_inf * PDF(t_mode).
  // Multiply the old max_infectiousness by this factor to preserve
  // previous infectiousness magnitudes after the normalisation fix.
  double peakScalingFactor() const { return std::exp(log_peak_); }

  std::unique_ptr<InfectiousnessCurve> clone() const override {
    return std::make_unique<GammaCurve>(max_inf_, shape_, rate_, shift_);
  }

 private:
  double max_inf_;
  double shape_;
  double rate_;
  double shift_;
  double normalization_constant_;
  double log_peak_;
};

// =============================================================================
// Exponential Decay Curve
// =============================================================================

class ExponentialDecayCurve : public InfectiousnessCurve {
 public:
  ExponentialDecayCurve(double initial_value, double decay_rate,
                        double delay = 0.0)
      : initial_(initial_value), decay_(decay_rate), delay_(delay) {}

  double evaluate(double t) const override {
    double relative_t = t - delay_;
    if (relative_t <= 0.0) return initial_;
    return initial_ * std::exp(-decay_ * relative_t);
  }

  std::unique_ptr<InfectiousnessCurve> clone() const override {
    return std::make_unique<ExponentialDecayCurve>(initial_, decay_, delay_);
  }

 private:
  double initial_;
  double decay_;
  double delay_;
};

// =============================================================================
// Lognormal Curve - Lognormal PDF scaled by max_infectiousness
// =============================================================================

class LognormalCurve : public InfectiousnessCurve {
 public:
  // mu and sigma are the mean and std in log-space
  LognormalCurve(double max_infectiousness, double mu, double sigma)
      : max_inf_(max_infectiousness), mu_(mu), sigma_(sigma) {
    norm_ = 1.0 / (sigma_ * std::sqrt(2.0 * M_PI));

    // log-PDF at the mode exp(mu - sigma^2); used to normalise peak to
    // max_infectiousness.
    double t_mode = std::exp(mu_ - sigma_ * sigma_);
    double z_mode = (std::log(t_mode) - mu_) / sigma_;
    log_peak_ = std::log(norm_) - 0.5 * z_mode * z_mode - std::log(t_mode);
  }

  double evaluate(double t) const override {
    if (t <= 0.0) return 0.0;
    double z = (std::log(t) - mu_) / sigma_;
    double log_val = std::log(norm_) - 0.5 * z * z - std::log(t);
    // Subtract log_peak_ so evaluate(t_mode) == max_inf_ (dimensionless).
    return max_inf_ * std::exp(log_val - log_peak_);
  }

  // Returns the old (pre-fix) peak value: max_inf * PDF(t_mode).
  double peakScalingFactor() const { return std::exp(log_peak_); }

  std::unique_ptr<InfectiousnessCurve> clone() const override {
    return std::make_unique<LognormalCurve>(max_inf_, mu_, sigma_);
  }

 private:
  double max_inf_;
  double mu_;
  double sigma_;
  double norm_;
  double log_peak_;
};

// =============================================================================
// Beta Curve - Beta PDF mapped to [0, duration], scaled by max_infectiousness
// =============================================================================

class BetaCurve : public InfectiousnessCurve {
 public:
  // alpha and beta are the shape parameters; duration maps [0,1] to
  // [0,duration]
  BetaCurve(double max_infectiousness, double alpha, double beta,
            double duration)
      : max_inf_(max_infectiousness),
        alpha_(alpha),
        beta_(beta),
        duration_(duration) {
    // log(1/B(a,b)) = lgamma(a+b) - lgamma(a) - lgamma(b)
    log_norm_ =
        std::lgamma(alpha_ + beta_) - std::lgamma(alpha_) - std::lgamma(beta_);

    // Mode of Beta(alpha, beta) on [0, duration]. Clamp to interior for
    // boundary cases (alpha <= 1 or beta <= 1).
    double x_mode;
    if (alpha_ > 1.0 && beta_ > 1.0) {
      x_mode = (alpha_ - 1.0) / (alpha_ + beta_ - 2.0);
    } else if (alpha_ > 1.0) {
      x_mode = 1.0 - 1e-9;
    } else {
      x_mode = 1e-9;
    }
    log_peak_ = log_norm_ + (alpha_ - 1.0) * std::log(x_mode) +
                (beta_ - 1.0) * std::log(1.0 - x_mode) - std::log(duration_);
  }

  double evaluate(double t) const override {
    if (t <= 0.0 || t >= duration_) return 0.0;
    double x = t / duration_;
    double log_val = log_norm_ + (alpha_ - 1.0) * std::log(x) +
                     (beta_ - 1.0) * std::log(1.0 - x) - std::log(duration_);
    // Subtract log_peak_ so evaluate(t_mode) == max_inf_ (dimensionless).
    return max_inf_ * std::exp(log_val - log_peak_);
  }

  // Returns the old (pre-fix) peak value: max_inf * PDF(t_mode).
  double peakScalingFactor() const { return std::exp(log_peak_); }

  std::unique_ptr<InfectiousnessCurve> clone() const override {
    return std::make_unique<BetaCurve>(max_inf_, alpha_, beta_, duration_);
  }

 private:
  double max_inf_;
  double alpha_;
  double beta_;
  double duration_;
  double log_norm_;
  double log_peak_;
};

// =============================================================================
// Linear Ramp Curve
// =============================================================================

class LinearRampCurve : public InfectiousnessCurve {
 public:
  LinearRampCurve(double start_value, double end_value, double duration)
      : start_(start_value), end_(end_value), duration_(duration) {}

  double evaluate(double t) const override {
    if (t <= 0.0) return start_;
    if (t >= duration_) return end_;
    return start_ + (end_ - start_) * (t / duration_);
  }

  std::unique_ptr<InfectiousnessCurve> clone() const override {
    return std::make_unique<LinearRampCurve>(start_, end_, duration_);
  }

 private:
  double start_;
  double end_;
  double duration_;
};

}  // namespace june
