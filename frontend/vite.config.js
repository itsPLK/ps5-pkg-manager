import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import { viteSingleFile } from 'vite-plugin-singlefile'
import fs from 'fs'
import path from 'path'
import { execSync } from 'child_process'

function getBuildInfo() {
  let version = process.env.VITE_APP_VERSION || ''
  if (!version) {
    try {
      const vh = fs.readFileSync(path.resolve(__dirname, '../include/version.h'), 'utf8')
      const m = vh.match(/#define\s+PKGMGR_VERSION\s+"([^"]+)"/)
      if (m) version = m[1]
    } catch (e) {}
    if (!version) version = '0.0.0-dev'
  }
  let commit = process.env.VITE_APP_COMMIT || ''
  if (!commit) {
    try {
      commit = execSync('git rev-parse --short HEAD', { encoding: 'utf8' }).trim()
    } catch (e) {}
    if (!commit) commit = 'unknown'
  }
  const date = process.env.VITE_APP_BUILD_DATE || (() => {
    const now = new Date()
    const pad = (n) => String(n).padStart(2, '0')
    return `${now.getUTCFullYear()}-${pad(now.getUTCMonth() + 1)}-${pad(now.getUTCDate())} ${pad(now.getUTCHours())}:${pad(now.getUTCMinutes())}:${pad(now.getUTCSeconds())} UTC`
  })()
  const title = `PKG Manager v${version} (${commit}, ${date}) by PLK`
  return { version, commit, date, title }
}

const buildInfo = getBuildInfo()

// Dev server backend, e.g. PKG_BACKEND=10.0.2.66:8844 npm run dev
const backend = process.env.PKG_BACKEND || '127.0.0.1:8844'
const backendHost = backend.replace(/:\d+$/, '')
const wsPort = process.env.PKG_BACKEND_WS_PORT || '18842'

function titlePlugin(title) {
  return {
    name: 'html-title-transform',
    transformIndexHtml(html) {
      if (html.includes('[[TITLE_PLACEHOLDER]]')) {
        return html.replace(/\[\[TITLE_PLACEHOLDER\]\]/g, title)
      }
      return html.replace(/<title>.*?<\/title>/, `<title>${title}</title>`)
    }
  }
}

// https://vitejs.dev/config/
export default defineConfig({
  plugins: [react(), viteSingleFile(), titlePlugin(buildInfo.title)],
  define: {
    __APP_VERSION__: JSON.stringify(buildInfo.version),
    __APP_COMMIT__: JSON.stringify(buildInfo.commit),
    __APP_BUILD_DATE__: JSON.stringify(buildInfo.date),
  },
  build: {
    target: ['es2015', 'safari12'],
    minify: 'terser',
    terserOptions: {
      compress: {
        drop_console: true,
        drop_debugger: true,
      },
      format: {
        comments: false,
      },
    },
    cssCodeSplit: false,
    assetsInlineLimit: 10000000,
  },
  server: {
    // PKG_NO_HMR=1 keeps code edits from resetting a page mid-install; reload to apply them.
    hmr: process.env.PKG_NO_HMR ? false : undefined,
    proxy: {
      '/api': {
        target: `http://${backend}`,
        changeOrigin: true
      },
      '/version': {
        target: `http://${backend}`,
        changeOrigin: true
      },
      '/cache.appcache': {
        target: `http://${backend}`,
        changeOrigin: true
      },
      // Direct-install upload socket; the dev client connects same-origin.
      '/ws/upload': {
        target: `ws://${backendHost}:${wsPort}`,
        ws: true,
        changeOrigin: true
      }
    }
  }
})

