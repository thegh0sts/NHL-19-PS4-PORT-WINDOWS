# NHL 19 online server image: the server files baked in (no dataset needed). Build from the
# repository root:  docker build -f packaging/truenas/Dockerfile -t nhl19-server .
# Then in compose.yaml use this image and drop the volume and working_dir lines.
FROM python:3-slim
WORKDIR /server
COPY tools/nhl19_server.py tools/ufc3_server.py tools/ufc3_auth.py tools/ufc3_games.py tools/blaze_tdf.py ./
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
USER 568:568
EXPOSE 42230/tcp 10041/tcp 40443/tcp 40080/tcp 10744/tcp 10745/tcp 17502/udp
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
  CMD python3 -c "import socket; socket.create_connection(('127.0.0.1', 10041), 3).close()"
CMD ["python3", "-u", "nhl19_server.py", "--bind", "0.0.0.0"]
