# NovaTerm Profile System Design

## 1. 目标

设计统一的终端 Profile 系统，用于管理：

-   本地 Shell
-   SSH Remote Session
-   Serial Console
-   WSL
-   Docker Container
-   Telnet 等连接方式

目标类似 Windows Terminal Profile + VS Code Remote。

# 2. 总体架构

                    NovaTerm

                        |
                 ProfileManager

                        |
         +--------------+--------------+
         |              |              |

     LocalProfile   RemoteProfile   SerialProfile

         |              |              |

     Process      SSH Client       Serial Driver

                        |
                        v

                 TerminalSession

                        |
                        v

              libvterm + QRhiRenderer

# 3. Profile数据模型

``` json
{
"name":"Ubuntu SSH",

"type":"ssh",

"connection":{
    "host":"192.168.1.100",
    "port":22,
    "username":"root"
},

"theme":"dark",

"scheme":"dracula",

"font":"JetBrains Mono"

}
```

# 4. Profile类型

## Local Shell

例如：

-   cmd
-   powershell
-   bash

``` json
{
"type":"local",

"command":"powershell.exe"
}
```

## SSH Profile

``` json
{
"type":"ssh",

"host":"server",

"port":22
}
```

## Serial Profile

``` json
{
"type":"serial",

"device":"COM3",

"baudrate":115200
}
```

# 5. 核心类设计

## ProfileManager

``` cpp
class ProfileManager
{

public:

bool loadProfiles();

Profile* getProfile(QString name);

bool saveProfile(Profile);


signals:

void profileChanged();

};
```

## TerminalProfile

``` cpp
class TerminalProfile
{

QString name;

ProfileType type;

QString shell;

QString theme;

QString scheme;

FontConfig font;

};
```

# 6. Session管理

    Profile

       |

    SessionManager

       |

    TerminalSession

       |

    TerminalBackend

Backend抽象：

``` cpp
class TerminalBackend
{

virtual bool start();

virtual void write();

virtual QByteArray read();

virtual void close();

};
```

实现：

    LocalBackend

    SSHBackend

    SerialBackend

# 7. 开发阶段

## Phase 1

实现：

-   Profile JSON
-   ProfileManager
-   Local Shell

## Phase 2

实现：

-   SSH Profile
-   libssh/libssh2接入

## Phase 3

实现：

-   Serial Console

## Phase 4

实现：

-   Session Tab管理
-   最近连接
-   快速启动

# 8. 设计约束

1.  Profile 不管理 Renderer。

2.  Renderer 不知道连接类型。

3.  Session 与 Profile 分离。

4.  Backend 与 UI 解耦。

5.  所有连接统一进入 TerminalSession。
