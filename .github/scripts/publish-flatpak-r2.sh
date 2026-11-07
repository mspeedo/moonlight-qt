#!/usr/bin/env bash
# Publish the fork's Flatpak OSTree repository to Cloudflare R2.
set -euo pipefail

die() { echo "::error::$*" >&2; exit 1; }

: "${GITHUB_OUTPUT:=/dev/null}"

case "${1:-}" in
  prepare)
    required=(R2_BUCKET R2_ENDPOINT R2_PUBLIC_URL R2_ACCESS_KEY_ID R2_SECRET_ACCESS_KEY FLATPAK_GPG_PRIVATE_KEY)
    missing=()
    for name in "${required[@]}"; do
      if [[ -z "${!name:-}" ]]; then missing+=("$name"); fi
    done
    if (( ${#missing[@]} )); then
      echo "enabled=false" >> "$GITHUB_OUTPUT"
      echo "::notice::R2 publication disabled. Set GitHub variables R2_BUCKET, R2_ENDPOINT, R2_PUBLIC_URL and secrets R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY, FLATPAK_GPG_PRIVATE_KEY. Missing: ${missing[*]}"
      exit 0
    fi

    [[ "$R2_ENDPOINT" == https://* ]] || die "R2_ENDPOINT must be an HTTPS S3 endpoint"
    [[ "$R2_PUBLIC_URL" == https://* ]] || die "R2_PUBLIC_URL must be an HTTPS public-bucket URL"

    # Cloudflare may display a bucket-suffixed S3 URL, while rclone
    # expects the account-level endpoint (it adds the bucket separately).
    normalized_endpoint="${R2_ENDPOINT%/}"
    normalized_endpoint="${normalized_endpoint%/${R2_BUCKET}}"
    if [[ "$normalized_endpoint" == */*/*/* ]]; then
      die "R2_ENDPOINT must be an account-level S3 URL, optionally followed by /$R2_BUCKET"
    fi
    export RCLONE_CONFIG_R2_ENDPOINT="$normalized_endpoint"
    echo "RCLONE_CONFIG_R2_ENDPOINT=$normalized_endpoint" >> "$GITHUB_ENV"
    echo "Normalized R2 S3 endpoint for bucket $R2_BUCKET"
    export GNUPGHOME="${RUNNER_TEMP}/moonlight-flatpak-gnupg"
    mkdir -p "$GNUPGHOME"
    chmod 700 "$GNUPGHOME"
    printf '%s\n' "$FLATPAK_GPG_PRIVATE_KEY" | gpg --batch --import >/dev/null
    mapfile -t key_fingerprints < <(gpg --batch --with-colons --list-secret-keys | awk -F: '$1 == "fpr" { print $10 }')
    [[ "${#key_fingerprints[@]}" -eq 1 ]] || die "Expected exactly one Flatpak signing secret key"
    key_id="${key_fingerprints[0]}"

    echo "GNUPGHOME=$GNUPGHOME" >> "$GITHUB_ENV"
    echo "enabled=true" >> "$GITHUB_OUTPUT"
    echo "key_id=$key_id" >> "$GITHUB_OUTPUT"

    # An empty R2 bucket is valid on the first build; subsequent runs restore
    # the previous OSTree objects so incremental static deltas can be generated.
    rclone lsf "r2:${R2_BUCKET}/" --max-depth 1 >/dev/null
    mkdir -p repo
    rclone copy "r2:${R2_BUCKET}/" repo/ \
      --exclude '/moonlight-fork.flatpakrepo' \
      --exclude '/moonlight-latest.json'
    echo "Existing R2 OSTree repository restored (empty on first publication)."
    ;;

  publish)
    [[ -n "${FLATPAK_GPG_KEY_ID:-}" ]] || die "Missing key fingerprint from prepare step"
    [[ -d repo ]] || die "Missing OSTree repository"
    flatpak build-update-repo \
      --generate-static-deltas \
      --gpg-sign="$FLATPAK_GPG_KEY_ID" \
      repo

    public_url="${R2_PUBLIC_URL%/}"
    public_key="$(gpg --batch --export "$FLATPAK_GPG_KEY_ID" | base64 -w 0)"
    [[ -n "$public_key" ]] || die "Failed to export GPG public key"
    cat > moonlight-fork.flatpakrepo <<EOF
[Flatpak Repo]
Title=Moonlight (mspeedo fork)
Comment=Moonlight Flatpak builds from feature/remote-sunshine-settings
Url=$public_url
Homepage=https://github.com/mspeedo/moonlight-qt
DefaultBranch=master
GPGKey=$public_key
EOF

    # Publish immutable content-addressed files first; mutable refs and signed
    # summary last. Do not delete old objects while clients may still use them.
    rclone copy repo/ "r2:${R2_BUCKET}/" \
      --exclude '/summary' \
      --exclude '/summary.sig' \
      --exclude '/refs/**' \
      --exclude '/tmp/**' \
      --exclude '/state/**' \
      --exclude '*.lock'
    rclone copy repo/refs/ "r2:${R2_BUCKET}/refs/"
    rclone copyto repo/summary.sig "r2:${R2_BUCKET}/summary.sig"
    rclone copyto repo/summary "r2:${R2_BUCKET}/summary"
    rclone copyto moonlight-fork.flatpakrepo "r2:${R2_BUCKET}/moonlight-fork.flatpakrepo"

    # Publish this index LAST, after all signed OSTree data is available.
    # Moonlight uses it for a real on-demand check rather than waiting for
    # the portal's periodic (typically 30-minute) remote polling cycle.
    ostree_commit="$(ostree --repo=repo rev-parse app/com.moonlight_stream.Moonlight/x86_64/master)"
    [[ "$ostree_commit" =~ ^[a-f0-9]{64}$ ]] || die "Could not resolve the published Moonlight OSTree commit"
    published_at="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    cat > moonlight-latest.json <<EOF
{"schema":1,"app_id":"com.moonlight_stream.Moonlight","arch":"x86_64","branch":"master","ostree_commit":"$ostree_commit","git_commit":"$GITHUB_SHA","published_at":"$published_at"}
EOF
    rclone copyto moonlight-latest.json "r2:${R2_BUCKET}/moonlight-latest.json"

    # Public access must be separately enabled on the R2 bucket.
    curl --fail --silent --show-error --location --retry 3 --retry-delay 2 \
      "$public_url/summary" --output /dev/null \
      || die "Uploaded but public summary unreachable. Enable public bucket access and check R2_PUBLIC_URL."
    curl --fail --silent --show-error --location \
      "$public_url/moonlight-fork.flatpakrepo" --output /dev/null \
      || die "Public Flatpak remote descriptor is unreachable"
    curl --fail --silent --show-error --location \
      "$public_url/moonlight-latest.json" --output /dev/null \
      || die "Public release index is unreachable"

    {
      echo '## Flatpak R2 publication'
      echo ''
      echo "- Repository: $public_url"
      echo "- Remote descriptor: $public_url/moonlight-fork.flatpakrepo"
      echo '- Signed commits and repository summary: yes'
      echo '- Incremental OSTree history: preserved'
      echo '- Latest release index: `$public_url/moonlight-latest.json`'
    } >> "$GITHUB_STEP_SUMMARY"
    ;;

  *)
    die "Usage: $0 prepare|publish"
    ;;
esac
