# lrn-http-proxy

클라이언트의 HTTP GET을 원본 서버로 중계하고, 재사용할 수 있는 작은 응답을 메모리에 보관하는 C 프록시다. 같은 파일을 반복 요청하면서 원본 서버에 실제로 몇 번 접근했는지, 응답이 만료되면 어떻게 달라지는지 확인할 수 있다.

## 실행

macOS/Linux의 C compiler·make·Python 3가 필요하다.

```sh
make setup
make demo
# 터미널 1: 프록시
make serve
# 터미널 2: Tiny 원본 서버
make -C webproxy-lab run-tiny TINY_PORT=8000
# 터미널 3: 같은 파일을 두 번 요청
curl --noproxy '' -x http://127.0.0.1:8080 http://127.0.0.1:8000/home.html
```

서버는 Ctrl-C로 종료한다. demo/test는 자신이 만든 프로세스만 종료한다.

## 입력에서 출력까지

명시적 HTTP proxy 요청 → bounded worker → URL·헤더 검증 → origin 연결 또는 LRU cache → 클라이언트

프록시는 loopback에만 바인딩한다. 기본 worker 8개, 대기 queue 32개이며 초과 연결은 닫는다. `PROXY_WORKERS=1..32`, `PROXY_TIMEOUT_MS=50..60000`으로 조정한다. 기본 timeout은 2초다. TCP 연결은 nonblocking connect+poll deadline, read/write는 socket 유휴 timeout을 사용한다.

전체 cache 1,049,000바이트, 객체 하나 102,400바이트(헤더 포함), 최대 16개 entry, LRU 퇴출이다. 200 응답에 `public, max-age=N`이 있고 Content-Length가 명확한 작은 응답만 저장한다. N은 1..86400 범위이며 Age·Date·수신 지연·보관 시간을 반영한다. Cookie·Authorization·Range·조건부 요청, private/no-cache/no-store/알 수 없는 cache directive, Set-Cookie·Vary 응답은 재사용하지 않는다. 끊긴 본문은 저장하지 않는다.

Tiny의 정적 파일은 `public, max-age=2`를 표시한다. 자동 테스트용 Python origin은 요청 횟수를 세어 hit·miss·expiry를 독립적으로 확인한다. `make demo` 출력은 MISS 1 → HIT 1 → EXPIRED 2다. stderr에는 key별 HIT/MISS가 나온다.

연결·중계·캐시는 [`proxy.c`](webproxy-lab/proxy.c), 정적 파일을 제공하는 Tiny 서버는 [`tiny.c`](webproxy-lab/tiny/tiny.c)에 있다. 요청 횟수를 세는 검증용 원본 서버는 [`test_proxy.py`](tests/test_proxy.py)에 들어 있다.

## 검증과 관찰

```sh
make test
```

실제 TCP 연결로 캐시 hit·miss·만료와 LRU 퇴출을 검사한다. 인증·쿠키가 있는 요청의 캐시 우회, 큰 응답, 원본의 조기 종료, timeout, 지원하지 않는 framing과 동시 연결도 다룬다. `make demo`의 원본 요청 횟수는 첫 요청에서 1, 캐시 hit에서 1, 만료 후에는 2가 된다.

## 지원 범위와 한계

GET, absolute `http://host:port/path` 요청과 HTTP/1.0·1.1 응답만 지원한다. IPv6 URL literal, origin-form 요청, GET 본문, chunked framing, HTTPS CONNECT/TLS, HTTP/2·3, WebSocket, POST, 인증 및 재검증 cache는 제외한다. 지원하지 않는 request framing은 400/501, origin framing은 502로 거절한다.

DNS 조회 자체는 OS resolver를 따르며 TCP connect deadline에 포함하지 않는다. read/write timeout은 전체 다운로드 기한이 아니라 유휴 기한이다. queue가 가득 차면 별도 503 본문 없이 연결을 닫는다. 이미 일부 응답을 전달한 뒤 원본이 끊기면 연결을 종료하고 cache에 저장하지 않는다. Tiny는 실습용 origin이며 범용 인터넷 서버로 운영하는 대상이 아니다.

캐시 기준은 [RFC 9111](https://www.rfc-editor.org/rfc/rfc9111.html)의 freshness/age 개념을 제한적으로 구현한다. RFC 전체 적합성을 주장하지 않는다.

## 출처와 기여

[woonyong-kr/SW_AI-W08-webproxy_lab](https://github.com/woonyong-kr/SW_AI-W08-webproxy_lab)에서 이어 받은 학습용 파생본이다. 기준 원본 revision은 `964163c7583369819af422968fcd2540b8e37e23`이다. 원본 과제·팀 코드와 이후 개인 확장을 구분하며, 개별 기여는 Git author와 diff로 확인한다. 기존 저작권 표시는 소스에 유지한다.

이 파생본에서는 worker 수와 timeout에 상한을 두고, 캐시 가능한 응답과 거절할 메시지 형식을 좁혔다. 이전 Echo·Tiny·프록시 실습 자료는 [정리 전 이력](https://github.com/woonyong-kr/lrn-http-proxy/tree/0580e06a40163a42e70b18d065f47687ed9f53bf)에 남아 있다.
