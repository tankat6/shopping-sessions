# Build the native CSV parser.
FROM debian:bookworm-slim AS native-builder
RUN apt-get update && apt-get install -y --no-install-recommends g++ \
    && rm -rf /var/lib/apt/lists/*
COPY native/fast_csv.cpp /src/fast_csv.cpp
RUN g++ -O3 -std=c++17 -pthread -static -s /src/fast_csv.cpp -o /fast_csv

# Runtime image.
FROM python:3.12-slim-bookworm

RUN useradd --uid 1000 --no-create-home --home-dir /scratch app

COPY requirements.txt /app/requirements.txt
RUN pip install --no-cache-dir -r /app/requirements.txt

COPY pipeline/ /app/pipeline/
COPY --from=native-builder /fast_csv /app/pipeline/fast_csv
COPY run.sh /app/run.sh
RUN chmod +x /app/run.sh

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /app
CMD ["/app/run.sh"]
