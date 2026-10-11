# Chatterino9 mod tracker, see server.mjs
FROM node:22-alpine3.24
WORKDIR /app
# Readable for any user: the container runs as the server user's id
COPY --chmod=0444 server.mjs ./
USER node
CMD ["node", "server.mjs"]
