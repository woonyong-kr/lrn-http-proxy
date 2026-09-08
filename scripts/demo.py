from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from test_proxy import servers, request, COUNTS

with servers() as (proxy, origin):
    for stage in ["MISS", "HIT", "EXPIRED"]:
        if stage == "EXPIRED":
            time.sleep(2.1)
        response = request(proxy, origin, "/fresh")
        print(
            stage,
            "origin_requests=",
            COUNTS["/fresh"],
            "body=",
            response.split(b"\r\n\r\n", 1)[1].decode(),
        )
    assert COUNTS["/fresh"] == 2
