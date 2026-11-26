#!/data/data/com.termux/files/usr/bin/bash
# One-shot: push project to GitHub, build the .geode in the cloud, install it.
set -e
cd "$(dirname "$0")"

echo "[1/6] Installing tools..."
pkg update -y
pkg install -y git gh unzip

echo "[2/6] Storage access (tap Allow)..."
termux-setup-storage || true

echo "[3/6] GitHub login..."
if ! gh auth status >/dev/null 2>&1; then
  gh auth login
fi
gh auth setup-git
USER_NAME=$(gh api user -q .login)
git config --global user.name "$USER_NAME"
git config --global user.email "${USER_NAME}@users.noreply.github.com"

echo "[4/6] Pushing project..."
[ -d .git ] || git init -b main
git add -A
git commit -m "Showcase Bot" || true
if git remote | grep -q origin; then
  git push -u origin main
else
  gh repo create showcasebot --private --source=. --remote=origin --push
fi

echo "[5/6] Building in the cloud (about 10 min)..."
sleep 15
RUN_ID=$(gh run list --limit 1 --json databaseId -q '.[0].databaseId')
gh run watch "$RUN_ID" --exit-status
rm -rf out
gh run download "$RUN_ID" -n "Build Output" -D out

echo "[6/6] Installing to Geode..."
MODS="$HOME/storage/shared/Android/media/com.geode.launcher/game/geode/mods"
mkdir -p "$MODS"
find out -name "*.geode" -exec cp -v {} "$MODS/" \;

echo "Done! Restart Geometry Dash and open a level."
