# 🌐 lrn-http-proxy

HTTP GET 요청을 중계하고 재사용 가능한 작은 응답을 캐시하는 C 프록시입니다. 원본 서버의 요청 횟수와 HIT/MISS 로그로 캐시 동작을 확인합니다.

[Proxy Wiki](https://docs.woonyong.com/wiki/computer-systems-network-topic-e8bae755299d/) · [핵심 구현](webproxy-lab/proxy.c)

## 실행

macOS/Linux의 C compiler, Make, Python 3가 필요합니다.

```sh
make setup
make demo
make test
```

데모는 첫 요청 `MISS 1` → 재사용 `HIT 1` → 만료 후 `EXPIRED 2`를 출력합니다. 직접 요청하려면 다음 명령을 각각 다른 터미널에서 실행합니다.

```sh
make serve
make -C webproxy-lab run-tiny TINY_PORT=8000
curl --noproxy '' -x http://127.0.0.1:8080 http://127.0.0.1:8000/home.html
```

서버는 Ctrl-C로 종료합니다. demo/test는 자신이 만든 프로세스만 종료합니다.

## 구현과 설계

명시적 proxy 요청 → worker → URL·헤더 검증 → 원본 연결 또는 LRU cache → 응답으로 이어집니다.

- [proxy.c](webproxy-lab/proxy.c): 기본 worker 8개·대기 queue 32개로 동시 요청을 제한합니다. loopback에 바인딩하며 `PROXY_WORKERS`, `PROXY_TIMEOUT_MS`로 조정합니다.
- 메모리 캐시는 전체 1,049,000바이트·객체 102,400바이트·16개 entry로 제한합니다. Content-Length가 명확한 작은 200 응답 중 `public, max-age`로 재사용이 허용된 응답만 저장합니다.
- 인증·쿠키·Range·조건부 요청과 Set-Cookie·Vary 응답은 캐시하지 않습니다. Age·Date·전송 지연과 보관 시간을 반영하며, 끊긴 본문은 저장하지 않습니다.
- [Tiny](webproxy-lab/tiny/tiny.c)는 정적 파일을 제공합니다. [TCP 통합 검사](tests/test_proxy.py)는 별도 원본 서버의 요청 횟수로 캐시·만료·LRU와 실패 조건을 확인합니다.

## 현재 범위

absolute `http://host:port/path` 형식의 GET을 지원합니다. CONNECT/TLS, chunked framing, POST, HTTP/2·3, WebSocket과 캐시 재검증은 제외합니다. 지원하지 않는 request framing은 400/501, origin framing은 502로 거절합니다.

TCP connect에는 deadline, read/write에는 유휴 timeout을 적용합니다. DNS 조회는 connect deadline 밖이며 전체 다운로드 시간에 상한을 두는 방식은 아닙니다. 대기 queue가 가득 차면 연결을 닫고, 이미 응답 일부를 전달한 뒤 원본이 끊기면 연결 종료로 처리합니다. [RFC 9111](https://www.rfc-editor.org/rfc/rfc9111.html)의 캐시 정책을 제한적으로 구현한 학습용 프로그램입니다.

## 출처와 기여

원본 `woonyong-kr/SW_AI-W08-webproxy_lab`의 `964163c7583369819af422968fcd2540b8e37e23`에서 이어 받은 학습용 파생본이다. 원본 과제·팀 코드와 이후 개인 확장은 Git author와 diff로 구분하며, 기존 저작권 표시는 소스에 유지한다. 원본 주소의 공개 접근이 제한돼 있어 자료는 아래 이력 링크로 확인할 수 있다.

이 파생본에서는 worker 수와 timeout에 상한을 두고, 캐시 가능한 응답과 거절할 메시지 형식을 좁혔다. 이전 Echo·Tiny·프록시 실습 자료는 [정리 전 이력](https://github.com/woonyong-kr/lrn-http-proxy/tree/0580e06a40163a42e70b18d065f47687ed9f53bf)에 남아 있다.
