import { app, BrowserWindow, ipcMain } from 'electron';
import path from 'path';
import { fileURLToPath } from 'url';
import { createServer } from 'http';
import { readFileSync, writeFileSync, existsSync, mkdirSync, readdirSync, unlinkSync } from 'fs';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

// Setup persistent storage folder
const MUSIC_DIR = path.join(app.getPath('music'), 'Melody');
if (!existsSync(MUSIC_DIR)) mkdirSync(MUSIC_DIR, { recursive: true });

// IPC Handlers for Offline Storage
ipcMain.handle('save-song', async (event, { id, base64 }) => {
  const filePath = path.join(MUSIC_DIR, `${id}.m4a`);
  writeFileSync(filePath, Buffer.from(base64, 'base64'));
  return filePath;
});

ipcMain.handle('read-song', async (event, id) => {
  const filePath = path.join(MUSIC_DIR, `${id}.m4a`);
  if (!existsSync(filePath)) return null;
  return readFileSync(filePath).toString('base64');
});

ipcMain.handle('delete-song', async (event, id) => {
  const filePath = path.join(MUSIC_DIR, `${id}.m4a`);
  if (existsSync(filePath)) unlinkSync(filePath);
});

ipcMain.handle('get-music-dir', () => MUSIC_DIR);

app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');
app.commandLine.appendSwitch('disable-features', 'PreloadMediaEngagementData, MediaEngagementBypassAutoplayPolicies');

// Serve the built dist folder over a local HTTP server so requests from the
// renderer go out with Origin: http://127.0.0.1:PORT instead of null (file://).
// Invidious and other audio servers reject null-origin requests server-side,
// which is why the packaged exe couldn't stream while dev mode (localhost:5173) worked fine.
const MIME = {
  '.html': 'text/html',
  '.js':   'application/javascript',
  '.css':  'text/css',
  '.svg':  'image/svg+xml',
  '.png':  'image/png',
  '.ico':  'image/x-icon',
  '.woff2':'font/woff2',
};

function serveDistLocally() {
  const distPath = path.join(__dirname, 'dist');
  return new Promise((resolve) => {
    const server = createServer((req, res) => {
      const urlPath = (req.url === '/' ? '/index.html' : req.url).split('?')[0];
      let filePath = path.join(distPath, urlPath);
      if (!existsSync(filePath)) filePath = path.join(distPath, 'index.html');
      const ext = path.extname(filePath);
      const contentType = MIME[ext] || 'application/octet-stream';
      try {
        res.writeHead(200, { 
          'Content-Type': contentType,
          'Access-Control-Allow-Origin': '*' 
        });
        res.end(readFileSync(filePath));
      } catch {
        res.writeHead(404);
        res.end();
      }
    });
    server.listen(0, 'localhost', () => {
      resolve(`http://localhost:${server.address().port}`);
    });
  });
}

async function createWindow() {
  const win = new BrowserWindow({
    width: 1200,
    height: 800,
    frame: false, // Frameless for Liquid Glass look
    transparent: true,
    webPreferences: {
      nodeIntegration: true,
      contextIsolation: false,
      webSecurity: false,
    },
    autoHideMenuBar: true,
    title: 'Melody Player',
  });

  // Handle Window Controls via IPC
  ipcMain.on('window-min', () => win.minimize());
  ipcMain.on('window-max', () => win.isMaximized() ? win.unmaximize() : win.maximize());
  ipcMain.on('window-close', () => win.close());

  if (!app.isPackaged) {
    win.loadURL('http://localhost:5173');
  } else {
    const url = await serveDistLocally();
    win.loadURL(url);
  }
}

app.whenReady().then(createWindow);

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});
