@echo off
chcp 65001 >nul
title ESP-Claw 同步官方更新

echo ========================================
echo   ESP-Claw 同步官方 master 更新
echo ========================================
echo.

cd /d "%~dp0"

echo [1/4] 拉取官方最新代码...
git fetch origin
if %errorlevel% neq 0 (
    echo [错误] fetch 失败，请检查网络连接
    pause
    exit /b 1
)

echo.
echo [2/4] 切换到 master 并更新...
git checkout master
git merge origin/master --ff-only
if %errorlevel% neq 0 (
    echo [错误] master 更新失败
    pause
    exit /b 1
)

echo.
echo [3/4] 切换回 dev/benben-custom 分支...
git checkout dev/benben-custom

echo.
echo [4/4] 将官方更新合并到你的分支（rebase）...
git rebase master
if %errorlevel% neq 0 (
    echo.
    echo [警告] rebase 遇到冲突，请手动解决冲突后执行：
    echo   git rebase --continue
    echo   或放弃本次 rebase：
    echo   git rebase --abort
    pause
    exit /b 1
)

echo.
echo ========================================
echo   同步完成！当前分支：dev/benben-custom
echo ========================================
echo.
git log --oneline -5
echo.
pause
