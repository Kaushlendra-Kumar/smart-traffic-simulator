# Running locally and deploying for free

The project has two parts that need different kinds of hosting:

| Part | What it is | Free host |
| --- | --- | --- |
| Frontend (`frontend/`) | Static files after `npm run build` | **Vercel** |
| Server (`traffic_server`) | A long-running C++ process that keeps one live simulation in memory and streams it over WebSocket | **Render** (free Docker web service) |

Vercel can't run the server: its functions run Node, Bun or Python only, WebSocket
connections close after at most 300 s on the free plan, and memory isn't shared between
instances. The server needs one process that stays up, so it goes to Render.

You can also skip Vercel entirely: the server serves the built frontend itself, so the
Render URL alone is a complete deployment (see option B).

## 1. Run locally (Windows)

The C++ build is tested on Linux (GCC 13, Clang 18). On Windows, use one of these:

### Option 1 — Docker Desktop (closest to production)

1. Install [Docker Desktop](https://www.docker.com/products/docker-desktop/) and start it.
2. In PowerShell, from the project folder:

   ```powershell
   docker compose up --build
   ```

   The first build takes several minutes (it compiles the server, runs all tests, and
   builds the frontend). Open http://localhost:8080.
3. Stop with `Ctrl+C`; `docker compose down` removes the container.

This builds exactly the image Render will run, so if it works here it will work there.

### Option 2 — WSL 2 (fastest edit–build loop)

```powershell
wsl --install -d Ubuntu-24.04      # once, then reboot
```

Inside Ubuntu (your D: drive is at `/mnt/d`; building from the Linux home folder is much
faster, so copy or clone the project there):

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build nodejs npm \
  nlohmann-json3-dev libasio-dev libspdlog-dev libgtest-dev libbenchmark-dev
cp -r /mnt/d/project/smart-traffic-simulator ~/ && cd ~/smart-traffic-simulator
cmake --preset release && cmake --build --preset release -j
ctest --preset release
(cd frontend && npm ci && npm run build)
./build/release/traffic_server
```

Open http://localhost:8080 in your Windows browser. For hot-reloading UI work run
`scripts/dev.sh` and open http://localhost:5173. (If Ubuntu's `nodejs` is older than 18,
install Node 22 with [nvm](https://github.com/nvm-sh/nvm).)

A native MSVC build hasn't been tested; it may need small portability fixes.

## 2. Push to GitHub

From PowerShell in the project folder (the repository on GitHub is currently empty):

```powershell
git remote add origin https://github.com/Kaushlendra-Kumar/smart-traffic-simulator.git
git branch -M main
git push -u origin main
```

If `git remote add` says the remote already exists, use
`git remote set-url origin https://github.com/Kaushlendra-Kumar/smart-traffic-simulator.git`.
If the folder isn't a git repository yet, run `git init`, `git add .` and
`git commit -m "Initial commit"` first.

The included GitHub Actions workflow then builds and tests everything on each push
(GCC, Clang, AddressSanitizer, ThreadSanitizer, frontend, Docker image).

## 3. Deploy the server on Render (free)

1. Sign up at https://render.com with your GitHub account.
2. **New → Blueprint**, select `smart-traffic-simulator`. Render reads `render.yaml` and
   creates a free Docker web service with a health check on `/api/v1/health`.
   (Alternatively **New → Web Service**, pick the repo, runtime *Docker*, instance type
   *Free*.)
3. Wait for the first build (several minutes). You get a URL like
   `https://smart-traffic-simulator.onrender.com`. Opening it already shows the full app.

The server reads the `PORT` variable Render provides. `EXPERIMENT_THREADS=1` keeps
experiments to one thread on the small free CPU.

What the free tier means in practice:

- The service **sleeps after 15 minutes without traffic** and takes about a minute to wake
  on the next visit. Open WebSocket connections count as traffic, so it stays awake while
  someone has the page open. The frontend shows a "waking up" message and connects
  automatically once the server is up.
- 750 free instance hours per month per workspace — enough for one always-on service.
- The simulation state and scenarios created through the API are lost when the service
  restarts or sleeps (no persistent disk on the free tier). The bundled scenarios ship in
  the image and are always available.
- Large experiments (many seeds, the city grid) run noticeably slower than on a laptop.

## 4. Deploy the frontend on Vercel (free)

1. Sign up at https://vercel.com with GitHub and **Add New → Project**, import the repo.
2. Set **Root Directory** to `frontend`. Vercel detects Vite; `frontend/vercel.json` sets
   the build command, output directory and single-page-app rewrites.
3. Under **Environment Variables** add
   `VITE_BACKEND_URL` = your Render URL, e.g. `https://smart-traffic-simulator.onrender.com`
   (no trailing slash).
4. Deploy. The Vercel site loads the UI from Vercel's CDN and talks to Render for the API
   (`https://…/api/v1`) and the live stream (`wss://…/ws`). The server already sends CORS
   headers for any origin.

`VITE_BACKEND_URL` is baked in at build time: after changing it, redeploy.

### Option B — Render only

Skip step 4 and share the Render URL. Same features, one deployment. Vercel's advantage
is that the page itself loads instantly from a CDN even while the server is waking up.

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| Page says "Waking up the simulation server…" for over 2 minutes | Open the Render dashboard → service → Logs. Check that `VITE_BACKEND_URL` is the `https://` Render URL |
| Vercel page loads but nothing connects | Open the browser console. A mixed-content error means `VITE_BACKEND_URL` starts with `http://`; use `https://` |
| `docker compose up` fails at `npm ci` | Commit `frontend/package-lock.json` |
| Render build fails in the test step | The image runs the full C++ test suite; see which test failed in the build log |
| Scenario you saved through the API disappeared | Expected on the free tier after a restart; add it to `config/scenarios/` and push |
