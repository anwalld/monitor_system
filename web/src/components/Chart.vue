<script setup>
// ECharts 折线图：series = [{ name, data: [[ts, v], …], yAxisIndex? }]
import { onMounted, onBeforeUnmount, ref, watch } from "vue";
import * as echarts from "echarts/core";
import { LineChart } from "echarts/charts";
import { GridComponent, TooltipComponent, LegendComponent, TitleComponent } from "echarts/components";
import { CanvasRenderer } from "echarts/renderers";

echarts.use([LineChart, GridComponent, TooltipComponent, LegendComponent, TitleComponent, CanvasRenderer]);

const props = defineProps({
  title: String,
  series: Array,
  yAxes: { type: Array, default: () => [{}] }, // [{ name, max, fmt }]
});
const el = ref(null);
let chart = null;
const dark = window.matchMedia("(prefers-color-scheme: dark)");

function render() {
  if (!chart) return;
  const text = getComputedStyle(document.documentElement).getPropertyValue("--muted").trim();
  const line = getComputedStyle(document.documentElement).getPropertyValue("--line").trim();
  chart.setOption(
    {
      title: { text: props.title, left: 8, top: 4, textStyle: { fontSize: 13, color: text, fontWeight: 500 } },
      tooltip: { trigger: "axis", valueFormatter: (v) => (v == null ? "—" : Number(v).toFixed(3)) },
      legend: { bottom: 2, textStyle: { color: text } },
      grid: { left: 52, right: props.yAxes.length > 1 ? 52 : 16, top: 36, bottom: 52 },
      xAxis: { type: "time", axisLabel: { color: text }, axisLine: { lineStyle: { color: line } } },
      yAxis: props.yAxes.map((a) => ({
        type: "value", name: a.name, max: a.max, min: 0, nameTextStyle: { color: text },
        axisLabel: { color: text, formatter: a.fmt }, splitLine: { lineStyle: { color: line } },
      })),
      series: props.series.map((s) => ({
        type: "line", showSymbol: false, connectNulls: false, lineStyle: { width: 1.5 }, ...s,
      })),
    },
    true,
  );
}

const resize = () => chart?.resize();
onMounted(() => {
  chart = echarts.init(el.value, null, { renderer: "canvas" });
  render();
  window.addEventListener("resize", resize);
  dark.addEventListener("change", render);
});
onBeforeUnmount(() => {
  window.removeEventListener("resize", resize);
  dark.removeEventListener("change", render);
  chart?.dispose();
});
watch(() => props.series, render, { deep: true });
</script>

<template><div ref="el" class="chart card"></div></template>
