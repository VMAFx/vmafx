// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

import "github.com/grafana/grafana-foundation-sdk/go/dashboard"

// exporterTagPrefix marks a dashboard built on a vendor GPU exporter; the
// contract check allows that exporter's series on it and nowhere else.
const exporterTagPrefix = "vmafx-exporter-"

// gpuExporter is one vendor's GPU exporter and what its dashboard reads, with
// the series names of the exporter's own reference.
type gpuExporter struct {
	key, title, description string
	// util is the device utilisation series, percent.
	util string
	// memRatio is the device memory in use as a share 0..1.
	memRatio string
	// legend names one device in the exporter's labels.
	legend string
	extra  []panelBuilder
}

// gpuExporters are the exporters VMAFx ships a dashboard for. Each is
// imported, or enabled in the chart, only where that exporter runs, so a
// cluster without it has no dashboard showing "No data".
func gpuExporters() []gpuExporter {
	return []gpuExporter{
		{
			key: "dcgm", title: "VMAFx GPUs (NVIDIA DCGM exporter)",
			description: "NVIDIA GPUs of the cluster from NVIDIA's dcgm-exporter (series of its etc/default-counters.csv): utilisation, memory, encoder and decoder engines, power.",
			util:        "DCGM_FI_DEV_GPU_UTIL",
			memRatio:    sel("DCGM_FI_DEV_FB_USED") + " / clamp_min(" + sel("DCGM_FI_DEV_FB_USED") + " + " + sel("DCGM_FI_DEV_FB_FREE") + ", 1)",
			legend:      "{{instance}} GPU {{gpu}}",
			extra: []panelBuilder{
				timeseriesPanel("Encoder and decoder use", "NVENC and NVDEC utilisation per GPU, percent: encode-time scoring pipelines load these engines.", "percent",
					query(sel("DCGM_FI_DEV_ENC_UTIL"), "{{instance}} GPU {{gpu}} encoder"),
					query(sel("DCGM_FI_DEV_DEC_UTIL"), "{{instance}} GPU {{gpu}} decoder")),
				timeseriesPanel("Power draw", "Power draw per GPU.", "watt",
					query(sel("DCGM_FI_DEV_POWER_USAGE"), "{{instance}} GPU {{gpu}}")),
			},
		},
		{
			key: "amd", title: "VMAFx GPUs (AMD device metrics exporter)",
			description: "AMD GPUs of the cluster from AMD's device-metrics-exporter (series of its grafana/dashboard_gpu.json): graphics activity, VRAM, power.",
			util:        "gpu_gfx_activity",
			memRatio:    sel("gpu_used_vram") + " / clamp_min(" + sel("gpu_total_vram") + ", 1)",
			legend:      "{{hostname}}[{{gpu_id}}]",
			extra: []panelBuilder{
				timeseriesPanel("Power draw", "Package power per GPU.", "watt",
					query(sel("gpu_package_power"), "{{hostname}}[{{gpu_id}}]")),
			},
		},
		{
			key: "intel", title: "VMAFx GPUs (Intel XPU Manager exporter)",
			description: "Intel GPUs of the cluster from Intel XPU Manager (series of its doc/Prometheus_Exported_Metrics.csv): GPU active time, media engines, memory, power.",
			util:        "xpum_engine_ratio",
			memRatio:    sel("xpum_memory_ratio") + " / 100",
			legend:      "{{pci_bdf}}/{{sub_dev}}",
			extra: []panelBuilder{
				timeseriesPanel("Media engine use", "Utilisation of the media (video) engines per GPU tile, percent.", "percent",
					query(sel("xpum_engine_group_ratio", `type="media"`), "{{pci_bdf}}/{{sub_dev}}")),
				timeseriesPanel("Power draw", "Power draw per GPU.", "watt",
					query(sel("xpum_power_watts"), "{{pci_bdf}}/{{sub_dev}}")),
			},
		},
	}
}

// exporterDashboard builds the dashboard of one exporter: its job and
// instance variables list the exporter's own scrape targets.
func exporterDashboard(e gpuExporter) *dashboard.DashboardBuilder {
	b := baseDashboard("vmafx-gpu-"+e.key, e.title, e.description, e.util, []string{tag, exporterTagPrefix + e.key})
	panels := append([]panelBuilder{
		timeseriesPanel("GPU utilisation", "Busy time of each GPU, percent. Low while scoring jobs queue for this backend means the nodes, not the GPUs, are the limit.", "percent",
			query(sel(e.util), e.legend)),
		timeseriesPanel("GPU memory in use", "Device memory in use as a share of the device's memory.", unitRatio,
			query(e.memRatio, e.legend)),
	}, e.extra...)
	return withRows(b, []row{{"GPUs", panels}})
}
