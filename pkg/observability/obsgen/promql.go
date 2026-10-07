// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

import (
	"fmt"
	"strings"

	"github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// Scope matchers every dashboard query carries: the Prometheus job and
// instance the dashboard's template variables select (dashboard-linter's
// target-job-rule and target-instance-rule).
const scopeMatchers = `job=~"$job",instance=~"$instance"`

// rateWindow is the range of every rate(), increase() and histogram query:
// Grafana's $__rate_interval (target-rate-interval-rule).
const rateWindow = "[$__rate_interval]"

// sel returns the selector of series with the scope matchers and extra
// matchers, e.g. sel(f.Name, `tenant=~"$tenant"`).
func sel(series string, extra ...string) string {
	return series + "{" + strings.Join(append([]string{scopeMatchers}, extra...), ",") + "}"
}

// fam returns the selector of a counter or gauge family. A histogram has no
// series under its bare name; the contract check (CheckExpr) refuses one.
func fam(f metricdef.Family, extra ...string) string {
	return sel(f.Name, extra...)
}

// rate is the per-second rate of a counter family, summed by the given labels.
func rate(f metricdef.Family, by string, extra ...string) string {
	return sumBy(by, "rate("+fam(f, extra...)+rateWindow+")")
}

// increase is the increase of a counter family over the window, summed by
// the given labels.
func increase(f metricdef.Family, by string, extra ...string) string {
	return sumBy(by, "increase("+fam(f, extra...)+rateWindow+")")
}

// sumBy wraps expr in sum by (labels), or sum when labels is empty.
func sumBy(labels, expr string) string {
	if labels == "" {
		return "sum(" + expr + ")"
	}
	return "sum by (" + labels + ") (" + expr + ")"
}

// quantile is the q-quantile of a histogram family over the window, per the
// given labels.
func quantile(q float64, f metricdef.Family, by string, extra ...string) string {
	group := "le"
	if by != "" {
		group = by + ", le"
	}
	return fmt.Sprintf("histogram_quantile(%g, sum by (%s) (rate(%s%s)))", q, group, bucket(f, extra...), rateWindow)
}

// bucket returns the _bucket selector of a histogram family.
func bucket(f metricdef.Family, extra ...string) string {
	return sel(f.Name+"_bucket", extra...)
}

// orZero makes an empty aggregation read 0 instead of "No data": a queue with
// no jobs has no per-tenant series.
func orZero(expr string) string {
	return expr + " or vector(0)"
}

// tenantMatcher filters by the dashboard's tenant variable.
const tenantMatcher = `tenant=~"$tenant"`
