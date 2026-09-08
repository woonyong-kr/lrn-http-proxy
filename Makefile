.PHONY: setup demo test serve
setup:
	$(MAKE) -C webproxy-lab build-all

demo: setup
	python3 scripts/demo.py

test: setup
	python3 -m unittest discover -s tests -v

serve: setup
	./webproxy-lab/proxy 8080
