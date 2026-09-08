# lrn-http-proxy

HTTP GET을 중계하는 동시성 캐시 프록시. 핵심 엔진을 실제 입력으로 실행하고 결과와 내부 동작을 확인하는 독립 프로그램이다.

## 실행

macOS/Linux의 C compiler·make·Python 3가 필요하다.

```sh
make setup
make test
make demo
# 터미널 1: 프록시
make serve
# 터미널 2: Tiny 원본 서버
make -C webproxy-lab run-tiny TINY_PORT=8000
# 터미널 3: 같은 파일을 두 번 요청
curl --noproxy '' -x http://127.0.0.1:8080 http://127.0.0.1:8000/home.html
```

대화형 서버는 해당 터미널에서 Ctrl-C로 종료한다. demo/test의 자식 프로세스는 실행기가 보유한 PID 또는 컨테이너 ID로만 종료한다. 다른 서버를 포트 번호로 찾아 일괄 종료하지 않는다. 준비된 Python 환경이 없으면 먼저 `make setup`을 실행한다.

## 입력에서 출력까지

명시적 HTTP proxy 요청 → bounded worker → URL·헤더 검증 → origin 연결 또는 LRU cache → 클라이언트

프록시는 loopback에만 바인딩한다. 기본 worker 8개, 대기 queue 32개이며 초과 연결은 닫는다. `PROXY_WORKERS=1..32`, `PROXY_TIMEOUT_MS=50..60000`으로 조정한다. 기본 timeout은 2초다. TCP 연결은 nonblocking connect+poll deadline, read/write는 socket 유휴 timeout을 사용한다.

전체 cache 1,049,000바이트, 객체 하나 102,400바이트(헤더 포함), 최대 16개 entry, LRU 퇴출이다. 200 응답에 `public, max-age=N`이 있고 Content-Length가 명확한 작은 응답만 저장한다. N은 1..86400 범위이며 Age·Date·수신 지연·보관 시간을 반영한다. Cookie·Authorization·Range·조건부 요청, private/no-cache/no-store/알 수 없는 cache directive, Set-Cookie·Vary 응답은 재사용하지 않는다. 끊긴 본문은 저장하지 않는다.

Tiny의 정적 파일은 `public, max-age=2`를 표시한다. 자동 테스트용 Python origin은 요청 횟수를 세어 hit·miss·expiry를 독립적으로 확인한다. `make demo` 출력은 MISS 1 → HIT 1 → EXPIRED 2다. stderr에는 key별 HIT/MISS가 나온다.

구현을 읽는 순서: `webproxy-lab/proxy.c`, `webproxy-lab/tiny/tiny.c`, `tests/test_proxy.py`.

## 검증과 관찰

실제 TCP 통합 테스트로 인증·cookie 우회, cache 용량과 LRU, hit/miss/만료, 큰 응답, 원본 조기 종료, timeout, framing 거절, 동시 요청·반복 연결 종료, Tiny 정적 파일 중계를 검증한다.

실행 환경·명령·exit code·원본 백업과 전체 결과는 이번 전환의 별도 작업 폴더에 기록한다. 새 기계에서는 같은 명령으로 직접 재검증한다. 수치가 기록되어 있다는 사실과 현재 실행 성공을 구분한다.

## 지원 범위와 한계

GET, absolute `http://host:port/path` 요청과 HTTP/1.0·1.1 응답만 지원한다. IPv6 URL literal, origin-form 요청, GET 본문, chunked framing, HTTPS CONNECT/TLS, HTTP/2·3, WebSocket, POST, 인증 및 재검증 cache는 제외한다. 지원하지 않는 request framing은 400/501, origin framing은 502로 거절한다.

DNS 조회 자체는 OS resolver를 따르며 TCP connect deadline에 포함하지 않는다. read/write timeout은 전체 다운로드 기한이 아니라 유휴 기한이다. queue가 가득 차면 별도 503 본문 없이 연결을 닫는다. 이미 일부 응답을 전달한 뒤 원본이 끊기면 연결을 종료하고 cache에 저장하지 않는다. Tiny는 실습용 origin이며 범용 인터넷 서버로 운영하는 대상이 아니다.

캐시 기준은 [RFC 9111](https://www.rfc-editor.org/rfc/rfc9111.html)의 freshness/age 개념을 제한적으로 구현한다. RFC 전체 적합성을 주장하지 않는다.

## 원본·학습 문서의 경계

[원본 아카이브와 기여 구분](archive/README.md)을 확인한다. 이 저장소는 실행 코드·테스트·사용법·설계 근거를 소유한다. WIKI는 개념 정본을 소유하며 기존 정본·공통 색인·배포 파일을 이 작업에서 수정하지 않는다. SQL·PintOS와 RepoLM/음성 서비스는 이 프로그램의 실행 의존성이 아니다.
