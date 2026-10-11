import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// In development the Vite server proxies the API and WebSocket to the C++ server.
const backend = process.env.TRAFFIC_BACKEND ?? "http://localhost:8080";

export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      "/api": backend,
      "/ws": { target: backend.replace(/^http/, "ws"), ws: true },
    },
  },
  test: { environment: "node" },
} as never);
