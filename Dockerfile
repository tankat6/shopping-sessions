# Build a static native binary.
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY src/ src/
RUN g++ -O3 -std=c++20 -mavx2 -mbmi -mbmi2 -mpopcnt -pthread -static -s \
    src/main.cpp -o /shopping_sessions

# Runtime image: no interpreter, no libraries, nothing to import at startup.
FROM debian:bookworm-slim
RUN useradd --uid 1000 --no-create-home --home-dir /scratch app
COPY --from=build /shopping_sessions /app/shopping_sessions
WORKDIR /app
CMD ["/app/shopping_sessions"]
