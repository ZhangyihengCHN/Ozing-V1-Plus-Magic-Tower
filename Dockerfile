ARG NGINX_IMAGE=nginx:alpine
FROM ${NGINX_IMAGE}

COPY docker/nginx.conf /etc/nginx/nginx.conf
COPY index.html style.css game.js game-data.js /opt/magic-tower/
RUN for file in /opt/magic-tower/*; do gzip -9 -c "$file" > "$file.gz"; done

USER nginx
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
  CMD wget -q -O /dev/null http://127.0.0.1:8080/healthz || exit 1
ENTRYPOINT ["nginx"]
CMD ["-g", "daemon off;"]
