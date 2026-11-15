import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";

// 开发时把 /api 与 /ws 代理到 Manager（VITE_MANAGER，默认 http://127.0.0.1:8080）
const manager = process.env.VITE_MANAGER || "http://127.0.0.1:8080";
// AI 服务（M8）：VITE_AI，默认 http://127.0.0.1:8000
const ai = process.env.VITE_AI || "http://127.0.0.1:8000";

export default defineConfig({
  plugins: [vue()],
  server: {
    proxy: {
      "/api/ai": ai,
      "/api": manager,
      "/ws": { target: manager, ws: true },
    },
  },
  build: { chunkSizeWarningLimit: 1200 },
});
