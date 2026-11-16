@echo off
rem CLI entry point for Windows: search.cmd "query"
rem Default: run the CLI inside the running `server` compose service.
rem SEARCH_CLI_LOCAL=C:\path\search_cli.exe runs a local binary instead.
setlocal
set "ROOT=%~dp0"
if defined SEARCH_CLI_LOCAL (
  if not exist "%SEARCH_CLI_LOCAL%" (
    echo error: SEARCH_CLI_LOCAL=%SEARCH_CLI_LOCAL% does not exist 1>&2
    exit /b 2
  )
  "%SEARCH_CLI_LOCAL%" %*
  exit /b %ERRORLEVEL%
)
where docker >nul 2>nul
if errorlevel 1 (
  echo error: docker not found. Install Docker Desktop and run: docker compose up -d 1>&2
  exit /b 2
)
pushd "%ROOT%"
docker compose ps --status running --services 2>nul | findstr /x /c:"server" >nul
if errorlevel 1 (
  popd
  echo error: the 'server' container is not running. Start it with: docker compose up -d 1>&2
  exit /b 2
)
docker compose exec -T server /app/search_cli %*
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
