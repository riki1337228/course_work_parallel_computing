# Python tooling image for benchmarks, plots and end-to-end checks
# (used by the `benchmark` and `e2e` services, profile "tools").
FROM python:3.12-slim
RUN pip install --no-cache-dir matplotlib==3.9.2 websocket-client==1.8.0
WORKDIR /app
COPY scripts ./scripts
ENV PYTHONUNBUFFERED=1 \
    MPLBACKEND=Agg
