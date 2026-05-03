:: Assumed to be run from root directory.

mkdir DS3OS
mkdir DS3OS\Loader
mkdir DS3OS\Server
mkdir DS3OS\Prerequisites
copy Resources\ReadMe.txt DS3OS\ReadMe.txt
xcopy /s /y Resources\Prerequisites DS3OS\Prerequisites

if exist Bin\x64_release\server\steam_appid.txt xcopy /s /y Bin\x64_release\server\steam_appid.txt DS3OS\Server\
xcopy /s /y Bin\x64_release\server\steam_api64.dll DS3OS\Server\
xcopy /s /y Bin\x64_release\server\WebUI\ DS3OS\Server\WebUI\
xcopy /s /y Bin\x64_release\server\Server.exe DS3OS\Server\
xcopy /s /y Bin\x64_release\server\Server.pdb DS3OS\Server\

xcopy /s /y Bin\x64_release\loader\ DS3OS\Loader\
