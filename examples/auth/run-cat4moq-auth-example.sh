#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${OPENMOQ_BUILD_DIR:-${ROOT_DIR}/build}"

ENDPOINT="${CAT4MOQ_ENDPOINT:-https://127.0.0.1:4433/moq}"
NAMESPACE="${CAT4MOQ_NAMESPACE:-cat4moq.example}"
TRACK="${CAT4MOQ_TRACK:-video}"
DRAFT="${CAT4MOQ_DRAFT:-16}"
DURATION_SECONDS="${CAT4MOQ_SECONDS:-3}"
TOKEN_ENCODING="${CAT4MOQ_TOKEN_ENCODING:-auto}"
TOKEN_WRAPPER="${CAT4MOQ_TOKEN_WRAPPER:-}"
AUTH_PROFILE="${CAT4MOQ_AUTH_PROFILE:-}"
AUTH_TOKEN_TYPE="${CAT4MOQ_AUTH_TOKEN_TYPE:-}"
INSECURE="${CAT4MOQ_INSECURE:-0}"

TOKEN_FILE="${CAT4MOQ_TOKEN_FILE:-}"
SETUP_TOKEN_FILE="${CAT4MOQ_SETUP_TOKEN_FILE:-}"
ACTION_TOKEN_FILE="${CAT4MOQ_ACTION_TOKEN_FILE:-}"
TOKEN_COMMAND="${CATAPULT_CAT4MOQ_COMMAND:-${CAT4MOQ_TOKEN_COMMAND:-}}"
RELAY_COMMAND="${MOQX_RELAY_CMD:-}"
DPOP_KEY_FILE="${CAT4MOQ_DPOP_KEY_FILE:-}"
DPOP_TOKEN_TYPE="${CAT4MOQ_DPOP_TOKEN_TYPE:-}"

if [[ -z "${TOKEN_FILE}${SETUP_TOKEN_FILE}${ACTION_TOKEN_FILE}${TOKEN_COMMAND}" ]]; then
    echo "Set CAT4MOQ_TOKEN_FILE, CAT4MOQ_SETUP_TOKEN_FILE/CAT4MOQ_ACTION_TOKEN_FILE, or CATAPULT_CAT4MOQ_COMMAND." >&2
    exit 2
fi


relay_pid=""
cleanup() {
    if [[ -n "${relay_pid}" ]]; then
        kill "${relay_pid}" >/dev/null 2>&1 || true
        wait "${relay_pid}" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DOPENMOQ_BUILD_EXAMPLES=ON
cmake --build "${BUILD_DIR}" --target openmoq-publisher-auth-example

if [[ -n "${RELAY_COMMAND}" ]]; then
    bash -lc "${RELAY_COMMAND}" &
    relay_pid="$!"
    sleep "${MOQX_RELAY_STARTUP_SECONDS:-2}"
fi

args=(
    "--endpoint" "${ENDPOINT}"
    "--namespace" "${NAMESPACE}"
    "--track" "${TRACK}"
    "--draft" "${DRAFT}"
    "--seconds" "${DURATION_SECONDS}"
    "--token-encoding" "${TOKEN_ENCODING}"
    "--insecure-skip-verify" "${INSECURE}"
)

if [[ -n "${TOKEN_WRAPPER}" ]]; then
    args+=("--token-wrapper" "${TOKEN_WRAPPER}")
elif [[ -z "${AUTH_PROFILE}" ]]; then
    AUTH_PROFILE="moqx-compat"
fi
if [[ -n "${AUTH_PROFILE}" ]]; then
    args+=("--auth-profile" "${AUTH_PROFILE}")
fi
if [[ -n "${AUTH_TOKEN_TYPE}" ]]; then
    args+=("--auth-token-type" "${AUTH_TOKEN_TYPE}")
fi

if [[ -n "${TOKEN_FILE}" ]]; then
    args+=("--token-file" "${TOKEN_FILE}")
fi
if [[ -n "${SETUP_TOKEN_FILE}" ]]; then
    args+=("--setup-token-file" "${SETUP_TOKEN_FILE}")
fi
if [[ -n "${ACTION_TOKEN_FILE}" ]]; then
    args+=("--action-token-file" "${ACTION_TOKEN_FILE}")
fi
if [[ -n "${TOKEN_COMMAND}" ]]; then
    args+=("--catapult-command" "${TOKEN_COMMAND}")
fi
if [[ -n "${DPOP_KEY_FILE}" ]]; then
    args+=("--dpop-key-file" "${DPOP_KEY_FILE}")
fi
if [[ -n "${DPOP_TOKEN_TYPE}" ]]; then
    args+=("--dpop-token-type" "${DPOP_TOKEN_TYPE}")
fi


"${BUILD_DIR}/openmoq-publisher-auth-example" "${args[@]}"
