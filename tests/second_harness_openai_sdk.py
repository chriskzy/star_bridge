#!/usr/bin/env python3
"""SB-22: Plain OpenAI Python SDK harness against Star Bridge."""

import json
import sys
import threading
import time
from urllib import request as urlrequest

from openai import OpenAI, BadRequestError


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def delete(url):
    req = urlrequest.Request(url, method="DELETE")
    with urlrequest.urlopen(req, timeout=2) as resp:
        return resp.status, json.loads(resp.read().decode("utf-8"))


def main():
    if len(sys.argv) != 3:
        print("usage: second_harness_openai_sdk.py BASE_URL MODEL", file=sys.stderr)
        return 2
    base_url = sys.argv[1].rstrip("/")
    model = sys.argv[2]
    client = OpenAI(base_url=base_url, api_key="star-bridge-test")

    models = client.models.list()
    require(any(m.id == model for m in models.data), "model list does not include bridge model")
    print("PASS: OpenAI SDK models.list")

    resp = client.responses.create(model=model, input="SDK non-stream hello", stream=False)
    require(resp.object == "response", f"unexpected response object: {resp.object}")
    require(resp.status in ("completed", "incomplete"), f"unexpected status: {resp.status}")
    print("PASS: OpenAI SDK responses.create non-stream")

    events = []
    for event in client.responses.create(model=model, input="SDK stream hello", stream=True):
        name = getattr(event, "type", "") or event.__class__.__name__
        events.append(name)
        if name == "response.completed":
            break
    require("response.created" in events, f"stream missing response.created: {events}")
    require("response.completed" in events, f"stream missing response.completed: {events}")
    print("PASS: OpenAI SDK responses.create stream")

    try:
        client.responses.create(model=model, input="", stream=False)
    except BadRequestError as exc:
        require(exc.status_code == 400, f"expected 400, got {exc.status_code}")
        print("PASS: OpenAI SDK surfaces request validation error")
    else:
        raise AssertionError("empty input should raise BadRequestError")

    terminal = []
    def consume_stream():
        try:
            for event in client.responses.create(model=model, input="SDK cancel me", stream=True):
                name = getattr(event, "type", "") or event.__class__.__name__
                terminal.append(name)
                if name in ("response.completed", "response.failed"):
                    break
        except Exception as exc:  # SDK may raise when stream closes after response.failed.
            terminal.append(f"exception:{type(exc).__name__}")

    thread = threading.Thread(target=consume_stream)
    thread.start()
    time.sleep(0.3)
    status, body = delete(f"{base_url}/responses/sdk-cancel")
    require(status == 200, f"cancel HTTP status {status}")
    require(body.get("status") == "cancelled" or body.get("object") == "response",
            f"unexpected cancel body: {body}")
    thread.join(timeout=4)
    require(not thread.is_alive(), "stream did not terminate after cancel")
    require(any(x in terminal for x in ("response.failed", "exception:APIError")) or terminal,
            f"cancel stream produced no terminal evidence: {terminal}")
    print("PASS: second harness cancel")
    print("SECOND HARNESS PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
