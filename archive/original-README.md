> 원본 시점의 README 사본. 아래 상대 경로와 명령은 당시 저장소 루트 기준이며 현재 실행법은 루트 README를 따른다.

# 📘 Docker + VSCode DevContainer 기반 C 개발 환경 구축 가이드 (WebProxyLab)

이 문서는 **Windows**와 **macOS** 사용자가 Docker와 VSCode DevContainer 기능을 활용하여 C 개발 및 디버깅 환경을 빠르게 구축할 수 있도록 도와줍니다.

[**주의**] 지난 주차와 다른 점만 하시려면 4장부터 7장만 보세요.
[**주의**] `webproxy-lab`은 tiny 웹 서버와 프록시 서버 두 가지를 모두 구현해야 하므로 디버깅 설정도 두 가지를 제공합니다. 이에 대한 설명은 7장에서 다루니 꼭 읽어보시기 바랍니다.

---

## 1. Docker란 무엇인가요?

**Docker**는 애플리케이션을 어떤 컴퓨터에서든 **동일한 환경에서 실행**할 수 있게 도와주는 **가상화 플랫폼**입니다.  

Docker는 다음 구성요소로 이루어져 있습니다:

- **도커 엔진**: 컨테이너를 실행하는 핵심 서비스
- **도커 이미지**: 컨테이너 생성에 사용되는 템플릿 (레시피 📃)
- **도커 컨테이너**: 이미지를 기반으로 생성된 실제 실행 환경 (요리 🍜)

### ✅ AWS EC2와 비교했을 때 다른 점

| 구분 | EC2 같은 VM | Docker 컨테이너 |
|------|-------------|-----------------|
| 실행 단위 | OS 포함 전체 | 애플리케이션 단위 |
| 실행 속도 | 느림 (수십 초 이상) | 매우 빠름 (거의 즉시) |
| 리소스 사용 | 무거움 | 가벼움 |

---

## 2. VSCode DevContainer란 무엇인가요?

**DevContainer**는 VSCode에서 Docker 컨테이너를 **개발 환경**처럼 사용할 수 있게 해주는 기능입니다.

- 코드를 실행하거나 디버깅할 때 **컨테이너 내부 환경에서 동작**
- 팀원 간 **환경 차이 없이 동일한 개발 환경 구성** 가능
- `.devcontainer` 폴더에 정의된 설정을 VSCode가 읽어 자동 구성

---

## 3. Docker Desktop 설치하기

1. Docker 공식 사이트에서 설치 파일 다운로드:  
   👉 [https://www.docker.com/products/docker-desktop](https://www.docker.com/products/docker-desktop)

2. 설치 후 Docker Desktop 실행  
   - Windows: Docker 아이콘이 트레이에 떠야 함  
   - macOS: 상단 메뉴바에 Docker 아이콘 확인

---

## 4. 프로젝트 파일 내려받기 (히스토리 없이)

터미널(CMD, PowerShell, zsh 등)에서 아래 명령어로 프로젝트 폴더만 내려받습니다:

```bash
git clone --depth=1 https://github.com/krafton-jungle/webproxy_lab_docker.git
```

- `--depth=1` 옵션은 git commit 히스토리를 생략하고 **최신 파일만 가져옵니다.**

### 📂 내려받은 뒤 폴더 구조 설명

```
webproxy_lab_docker/
├── .devcontainer/
│   ├── devcontainer.json      # VSCode에서 컨테이너 환경 설정
│   └── Dockerfile             # C 개발 환경 이미지 정의
│
├── .vscode/
│   ├── launch.json            # 디버깅 설정 (F5 실행용)
│   └── tasks.json             # 컴파일 자동화 설정
│
├── webproxy-lab
│   ├── tiny                    # tiny 웹 서버 구현 폴더
│   │  ├── cgi-bin              # tiny 웹 서버를 테스트하기 위한 동적 컨텐츠를 구현하기 위한 폴더
│   │  ├── home.html            # tiny 웹 서버를 테스트하기 위한 정적 HTML 파일
│   │  ├── tiny.c               # tiny 웹 서버 구현 파일
│   │  └── Makefile             # tiny 웹 서버를 컴파일하기 위한 파일
│   ├── Makefile                # proxy 웹 서버를 컴파일하기 위한 파일
│   └── proxy.c                 # proxy 웹 서버 구현 파일
│
└── README.md  # 설치 및 사용법 설명 문서
```
---

## 5. VSCode에서 해당 프로젝트 폴더 열기

1. VSCode를 실행
2. `파일 → 폴더 열기`로 방금 클론한 `webproxy_lab_docker` 폴더를 선택

---

## 6. 개발 컨테이너에서 열기

1. VSCode에서 `Ctrl+Shift+P` (Windows/Linux) 또는 `Cmd+Shift+P` (macOS)를 누릅니다.
2. 명령어 팔레트에서 `Dev Containers: Reopen in Container`를 선택합니다.
3. 이후 컨테이너가 자동으로 실행되고 빌드됩니다. 처음 컨테이너를 열면 빌드에 시간이 조금 걸릴 수 있습니다. 빌드가 끝나면 프로젝트는 **컨테이너 내부에서 실행됩니다**.

---

## 7. C 파일에 브레이크포인트 설정 후 디버깅 (F5)

이제 본격적으로 문제를 풀 시간입니다. `webproxy-lab/README.md` 파일을 참고해서 웹프록시 문제를 풀어보세요.
구현 순서는 tiny 웹 서버(`webproxy-lab/tiny/tiny.c`)를 CS:APP 책의 코드를 참고해 먼저 완성하고, 프록시 서버(`webproxy-lab/proxy.c`)를 구현한 뒤 마지막에 `webproxy-lab/mdriver`를 실행해 70점 만점을 목표로 진행하면 됩니다.

C 언어로 문제를 풀다가 디버깅이 필요하면 소스 코드에 중단점을 설정한 뒤 키보드에서 `F5`를 눌러 디버깅을 시작할 수 있습니다. 디버깅 설정은 tiny 서버용과 프록시 서버용 두 가지가 제공되며, 각각 "Tiny 웹 서버 디버그", "프록시 서버 디버그"라는 이름을 가집니다. 두 설정 중 원하는 항목을 선택한 뒤 `F5`를 누르면 해당 서버가 디버그 모드로 실행됩니다.

* 기본적으로 "Tiny 웹 서버 디버그"는 tiny 서버를 실행할 때 포트 `8000`을 사용하고, "프록시 서버 디버그"는 `4500`을 사용합니다. 해당 포트를 이미 다른 프로세스가 사용 중이라면 `launch.json` 파일에서 다른 포트로 바꾼 뒤 디버깅을 진행하세요.


---

## 8. 새로운 Git 리포지토리에 커밋하고 푸시하기

금주 프로젝트를 개인 Git 리포와 같은 다른 리포지토리에 업로드하려면, 기존 Git 연결을 제거하고 새롭게 초기화해야 합니다.

### ✅ 완전히 새로운 Git 리포로 업로드하는 방법

아래 명령어를 순서대로 실행하세요:

```bash
rm -rf .git
git init
git remote add origin https://github.com/myusername/my-new-repo.git
git add .
git commit -m "초기 설정"
git push -u origin main
```

### 📌 설명

- `rm -rf .git`: 기존 Git 기록과 연결을 완전히 삭제합니다.
- `git init`: 현재 폴더를 새로운 Git 리포지토리로 초기화합니다.
- `git remote add origin ...`: 새로운 리포지토리 주소를 origin으로 등록합니다.
- `git add .` 및 `git commit`: 모든 파일을 커밋합니다.
- `git push`: 새로운 리포에 최초 업로드(Push)합니다.

이 과정을 거치면 기존 리포와의 연결은 완전히 제거되고, **새로운 독립적인 프로젝트로 관리**할 수 있습니다.
