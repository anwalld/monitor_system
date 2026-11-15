import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";

// 开发时把 /api 与 /ws 代理到 Manager（VITE_MANAGER，默认 http://127.0.0.1:8080）
const manager = process.env.VITE_MANAGER || "http://127.0.0.1:8080";

export default defineConfig({
  plugins: [vue()],
  server: {
    proxy: {
      "/api": manager,
      "/ws": { target: manager, ws: true },
    },
  },
  build: { chunkSizeWarningLimit: 1200 },
});
