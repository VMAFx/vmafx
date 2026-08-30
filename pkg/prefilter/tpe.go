// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Tree-structured Parzen Estimator — the search engine behind the joint
// deband + CRF sweep.
//
// # Why a native implementation
//
// The Python original delegates to Optuna's TPESampler. Go's only
// Optuna-lineage library, github.com/c-bata/goptuna, pulls gorm plus the
// MySQL, Postgres and cgo-SQLite drivers into the dependency graph for a
// storage layer a one-shot CLI never touches — that is a heavy, cgo-coupling
// dependency for a sampler that is a few hundred lines of arithmetic. So the
// sampler is implemented here, against the published algorithm.
//
// # The algorithm (Bergstra et al. 2011, "Algorithms for Hyper-Parameter
// Optimization", §4; Optuna's TPESampler follows the same construction)
//
//  1. The first StartupTrials proposals are drawn uniformly at random, to
//     seed the estimator with observations spanning the space.
//  2. Thereafter the observed trials are split by objective value at the
//     gamma quantile into l (the "good" set, below the split) and g (the
//     rest).
//  3. Each set is turned into a Parzen estimator per dimension: a mixture of
//     truncated Gaussians, one centred on each observation plus one broad
//     prior centred on the range's midpoint.
//  4. EICandidates points are drawn from l, and the one maximising
//     log l(x) - log g(x) is proposed. That ratio is monotone in Expected
//     Improvement, which is why maximising it maximises EI.
//
// # Parity caveat
//
// The trial-by-trial trajectory does NOT match Optuna's for the same seed,
// and cannot: the two use different RNG streams and differ in the details of
// their bandwidth heuristics and weighting schedules. What IS preserved is
// the search's contract — the same search space, the same objective, the
// same convergence behaviour, and a fully deterministic run for a given seed.
// The emitted JSON schema is identical either way. See the package's
// AGENTS.md note.

package prefilter

import (
	"math"
	"math/rand"
	"sort"
)

// Dimension is one axis of the search space.
type Dimension struct {
	Name string
	Kind KnobKind
	Lo   float64
	Hi   float64
}

// IsIntegral reports whether the dimension only takes integer values.
func (d Dimension) IsIntegral() bool {
	return d.Kind == KindInt || d.Kind == KindBool || d.Kind == KindEnum
}

// TPEConfig tunes the sampler. The defaults match Optuna's.
type TPEConfig struct {
	// StartupTrials is how many proposals are drawn uniformly before the
	// estimator takes over.
	StartupTrials int
	// EICandidates is how many points are drawn from l per proposal.
	EICandidates int
	// Gamma is the fraction of observations that go into the good set l.
	// The split size is ceil(Gamma * n), capped at 25 — Optuna's
	// default_gamma. The cap keeps l small enough to stay informative as n
	// grows, and keeps the prior component's share of the mixture small
	// enough that the estimator actually concentrates.
	Gamma float64
	// PriorWeight is the weight of the broad prior component relative to
	// each observation's component.
	PriorWeight float64
}

// DefaultTPEConfig returns Optuna's defaults.
func DefaultTPEConfig() TPEConfig {
	return TPEConfig{
		StartupTrials: 10,
		EICandidates:  24,
		Gamma:         0.10,
		PriorWeight:   1.0,
	}
}

// observation is one completed trial.
type observation struct {
	params    map[string]float64
	objective float64
}

// TPESampler proposes points in a fixed search space.
//
// It is not safe for concurrent use; the search loop is sequential by
// construction (each proposal depends on every prior observation).
type TPESampler struct {
	dims []Dimension
	cfg  TPEConfig
	rng  *rand.Rand
	obs  []observation
}

// NewTPESampler builds a sampler over dims with the given seed.
func NewTPESampler(dims []Dimension, cfg TPEConfig, seed int64) *TPESampler {
	if cfg.StartupTrials <= 0 {
		cfg.StartupTrials = DefaultTPEConfig().StartupTrials
	}
	if cfg.EICandidates <= 0 {
		cfg.EICandidates = DefaultTPEConfig().EICandidates
	}
	if cfg.Gamma <= 0.0 || cfg.Gamma >= 1.0 {
		cfg.Gamma = DefaultTPEConfig().Gamma
	}
	if cfg.PriorWeight <= 0.0 {
		cfg.PriorWeight = DefaultTPEConfig().PriorWeight
	}
	return &TPESampler{
		dims: dims,
		cfg:  cfg,
		//nolint:gosec // G404: this RNG drives a reproducible hyper-parameter
		// search, not a security decision. A seeded, deterministic stream is
		// the requirement (--seed must reproduce a run); crypto/rand would
		// make runs unreproducible.
		rng: rand.New(rand.NewSource(seed)),
	}
}

// Observe records a completed trial.
func (s *TPESampler) Observe(params map[string]float64, objective float64) {
	copied := make(map[string]float64, len(params))
	for k, v := range params {
		copied[k] = v
	}
	s.obs = append(s.obs, observation{params: copied, objective: objective})
}

// Trials returns how many observations the sampler holds.
func (s *TPESampler) Trials() int { return len(s.obs) }

// Suggest proposes the next point.
func (s *TPESampler) Suggest() map[string]float64 {
	if len(s.obs) < s.cfg.StartupTrials {
		return s.sampleUniform()
	}
	below, above := s.split()
	if len(below) == 0 || len(above) == 0 {
		return s.sampleUniform()
	}

	proposal := make(map[string]float64, len(s.dims))
	for _, dim := range s.dims {
		lEst := newParzen(observedValues(below, dim.Name), dim, s.cfg.PriorWeight)
		gEst := newParzen(observedValues(above, dim.Name), dim, s.cfg.PriorWeight)

		bestScore := math.Inf(-1)
		best := dim.Lo
		for i := 0; i < s.cfg.EICandidates; i++ {
			candidate := lEst.sample(s.rng)
			score := lEst.logPDF(candidate) - gEst.logPDF(candidate)
			if score > bestScore {
				bestScore, best = score, candidate
			}
		}
		proposal[dim.Name] = quantise(best, dim)
	}
	return proposal
}

// sampleUniform draws one point uniformly from the space.
func (s *TPESampler) sampleUniform() map[string]float64 {
	out := make(map[string]float64, len(s.dims))
	for _, dim := range s.dims {
		out[dim.Name] = quantise(dim.Lo+s.rng.Float64()*(dim.Hi-dim.Lo), dim)
	}
	return out
}

// split partitions the observations by objective into the good set l and the
// rest g.
//
// The split size follows Optuna's default_gamma: ceil(gamma * n) capped at
// 25. The cap matters for concentration as much as for informativeness — the
// broad prior always contributes one component to the mixture, so a small l
// would hand the prior a large share of the sampling mass and the search
// would stay close to uniform.
func (s *TPESampler) split() (below, above []observation) {
	sorted := make([]observation, len(s.obs))
	copy(sorted, s.obs)
	sort.SliceStable(sorted, func(i, j int) bool {
		return sorted[i].objective < sorted[j].objective
	})
	n := len(sorted)
	nBelow := int(math.Ceil(s.cfg.Gamma * float64(n)))
	if nBelow > 25 {
		nBelow = 25
	}
	if nBelow < 1 {
		nBelow = 1
	}
	if nBelow >= n {
		nBelow = n - 1
	}
	if nBelow < 1 {
		return nil, nil
	}
	return sorted[:nBelow], sorted[nBelow:]
}

// observedValues extracts one dimension's observed values.
func observedValues(obs []observation, name string) []float64 {
	out := make([]float64, 0, len(obs))
	for _, o := range obs {
		if v, ok := o.params[name]; ok {
			out = append(out, v)
		}
	}
	return out
}

// quantise rounds and clamps a proposal onto the dimension's lattice.
func quantise(v float64, dim Dimension) float64 {
	if v < dim.Lo {
		v = dim.Lo
	}
	if v > dim.Hi {
		v = dim.Hi
	}
	if dim.IsIntegral() {
		v = math.Round(v)
		if v < dim.Lo {
			v = math.Ceil(dim.Lo)
		}
		if v > dim.Hi {
			v = math.Floor(dim.Hi)
		}
	}
	return v
}

// parzen is a mixture of truncated Gaussians over [lo, hi].
type parzen struct {
	mus     []float64
	sigmas  []float64
	weights []float64
	lo      float64
	hi      float64
}

// newParzen fits the estimator to observations on dim.
//
// Bandwidths follow the Bergstra heuristic: each component's sigma is the
// larger gap to its sorted neighbours, floored so a dense cluster cannot
// collapse to a spike and capped at the full range. The prior component is
// centred on the midpoint with sigma equal to the whole range, which keeps
// the estimator from ever assigning zero density inside the box.
func newParzen(values []float64, dim Dimension, priorWeight float64) *parzen {
	span := dim.Hi - dim.Lo
	if span <= 0 {
		span = 1.0
	}
	p := &parzen{lo: dim.Lo, hi: dim.Hi}

	// Prior component first, so an empty observation set still yields a
	// proper distribution.
	p.mus = append(p.mus, (dim.Lo+dim.Hi)/2.0)
	p.sigmas = append(p.sigmas, span)
	p.weights = append(p.weights, priorWeight)

	if len(values) > 0 {
		sorted := make([]float64, len(values))
		copy(sorted, values)
		sort.Float64s(sorted)

		minSigma := span / math.Min(100.0, float64(len(sorted)+1))
		for i, v := range sorted {
			left, right := span, span
			if i > 0 {
				left = v - sorted[i-1]
			}
			if i < len(sorted)-1 {
				right = sorted[i+1] - v
			}
			sigma := math.Max(left, right)
			sigma = math.Max(sigma, minSigma)
			sigma = math.Min(sigma, span)
			p.mus = append(p.mus, v)
			p.sigmas = append(p.sigmas, sigma)
			p.weights = append(p.weights, 1.0)
		}
	}

	total := 0.0
	for _, w := range p.weights {
		total += w
	}
	for i := range p.weights {
		p.weights[i] /= total
	}
	return p
}

// sample draws one value from the mixture, truncated to [lo, hi].
//
// Truncation uses inverse-CDF sampling within the chosen component, which
// terminates in constant time — a rejection loop can stall when the component
// sits far outside the box.
func (p *parzen) sample(rng *rand.Rand) float64 {
	u := rng.Float64()
	acc := 0.0
	idx := len(p.weights) - 1
	for i, w := range p.weights {
		acc += w
		if u <= acc {
			idx = i
			break
		}
	}
	mu, sigma := p.mus[idx], p.sigmas[idx]
	loCDF := normalCDF((p.lo - mu) / sigma)
	hiCDF := normalCDF((p.hi - mu) / sigma)
	if hiCDF <= loCDF {
		return mu
	}
	q := loCDF + rng.Float64()*(hiCDF-loCDF)
	v := mu + sigma*normalPPF(q)
	return math.Max(p.lo, math.Min(p.hi, v))
}

// logPDF returns the mixture's log density at x.
func (p *parzen) logPDF(x float64) float64 {
	logs := make([]float64, 0, len(p.mus))
	for i := range p.mus {
		mu, sigma := p.mus[i], p.sigmas[i]
		z := (x - mu) / sigma
		norm := normalCDF((p.hi-mu)/sigma) - normalCDF((p.lo-mu)/sigma)
		if norm <= 0 {
			continue
		}
		logComponent := -0.5*z*z - math.Log(sigma) - 0.5*math.Log(2.0*math.Pi) - math.Log(norm)
		logs = append(logs, math.Log(p.weights[i])+logComponent)
	}
	return logSumExp(logs)
}

// logSumExp computes log(sum(exp(v))) without overflow.
func logSumExp(values []float64) float64 {
	if len(values) == 0 {
		return math.Inf(-1)
	}
	maxV := math.Inf(-1)
	for _, v := range values {
		if v > maxV {
			maxV = v
		}
	}
	if math.IsInf(maxV, -1) {
		return maxV
	}
	sum := 0.0
	for _, v := range values {
		sum += math.Exp(v - maxV)
	}
	return maxV + math.Log(sum)
}

// normalCDF is the standard normal CDF.
func normalCDF(z float64) float64 {
	return 0.5 * math.Erfc(-z/math.Sqrt2)
}

// normalPPF is the standard normal inverse CDF (probit), via the Acklam
// rational approximation refined by one Halley step against erfc.
//
// The upper half is evaluated by the symmetry PPF(p) = -PPF(1-p) rather than
// directly: the Halley step's residual 0.5*erfc(-x/sqrt2) - p cancels
// catastrophically as p approaches 1 (both terms approach 1), costing several
// digits in the far upper tail. Reflecting keeps every evaluation in the
// well-conditioned lower half, and makes the function exactly antisymmetric,
// which the truncated-normal sampler relies on for symmetric components.
func normalPPF(p float64) float64 {
	switch {
	case p <= 0.0:
		return math.Inf(-1)
	case p >= 1.0:
		return math.Inf(1)
	case p > 0.5:
		return -normalPPF(1.0 - p)
	}

	// Acklam's coefficients.
	a := [6]float64{
		-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
		1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00,
	}
	b := [5]float64{
		-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
		6.680131188771972e+01, -1.328068155288572e+01,
	}
	c := [6]float64{
		-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
		-2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00,
	}
	d := [4]float64{
		7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
		3.754408661907416e+00,
	}
	const pLow = 0.02425
	pHigh := 1.0 - pLow

	var x float64
	switch {
	case p < pLow:
		q := math.Sqrt(-2 * math.Log(p))
		x = (((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q + c[5]) /
			((((d[0]*q+d[1])*q+d[2])*q+d[3])*q + 1)
	case p <= pHigh:
		q := p - 0.5
		r := q * q
		x = (((((a[0]*r+a[1])*r+a[2])*r+a[3])*r+a[4])*r + a[5]) * q /
			(((((b[0]*r+b[1])*r+b[2])*r+b[3])*r+b[4])*r + 1)
	default:
		q := math.Sqrt(-2 * math.Log(1-p))
		x = -(((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q + c[5]) /
			((((d[0]*q+d[1])*q+d[2])*q+d[3])*q + 1)
	}

	// One Halley refinement step against the true CDF.
	e := 0.5*math.Erfc(-x/math.Sqrt2) - p
	u := e * math.Sqrt(2*math.Pi) * math.Exp(x*x/2)
	return x - u/(1+x*u/2)
}
