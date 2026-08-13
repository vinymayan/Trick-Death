import { defineConfig } from 'vite'
import solid from 'vite-plugin-solid'

export default defineConfig({
    plugins: [solid()],
    base: './',
    build: {
        assetsDir: '',
        rollupOptions: {
            output: {
                // Remove os hashes [hash] dos nomes dos arquivos
                entryFileNames: `[name].js`,
                chunkFileNames: `[name].js`,
                assetFileNames: `[name].[ext]`
            }
        }
    }
})