/// <reference types="vite/client" />

interface ImportMetaEnv {
  readonly VITE_BACKEND_URL?: string;
  readonly VITE_ENGINE?: "wasm" | "server";
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}
