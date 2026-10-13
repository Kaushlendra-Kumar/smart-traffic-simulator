import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// In development the Vite server proxies the API and WebSocket to the C++ server.
const backend = process.env.TRAFFIC_BACKEND ?? "http://localhost:8080";

export default defineConfig({
  plugins: [react()],
  // Workers are ES modules (they import the WebAssembly engine).
  worker: { format: "es" },
  server: {
    port: 5173,
    // The scenario library lives in ../config and is bundled into the static build.
    fs: { allow: [".."] },
    proxy: {
      "/api": backend,
      "/ws": { target: backend.replace(/^http/, "ws"), ws: true },
    },
  },
  test: { environment: "node" },
} as never);
