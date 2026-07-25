import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";
import { viteSingleFile } from "vite-plugin-singlefile";

// The build inlines everything into dist/index.html so the C++ binary can
// embed one file (cmake/EmbedFile.cmake).
export default defineConfig({
  plugins: [vue(), viteSingleFile()],
  server: {
    // `npm run dev` against a locally running mxl-decklink instance.
    proxy: {
      "/api": "http://127.0.0.1:8080",
      "/statusz": "http://127.0.0.1:8080",
      "/metrics": "http://127.0.0.1:8080",
    },
  },
});
