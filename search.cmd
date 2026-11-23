@echo off
rem CLI client wrapper for Windows:  search "artificial intelligence" --limit 10
rem The C++ client is built inside the search-server image, so no local compiler is needed.
docker compose exec -T search-server search %*
