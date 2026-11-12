# Qualification runtime: terminal data and Python fixtures, no development libraries.
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends python3.11 ncurses-base ncurses-term \
    && ln -s /usr/bin/python3.11 /usr/local/bin/python3 \
    && rm -rf /var/lib/apt/lists/*
