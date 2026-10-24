# deepfetch

Системная информация в терминале с живыми данными. Запускается как fastfetch и работает в фоне.

## Установка

    curl -fsSL https://raw.githubusercontent.com/deepsytxt/deepfetch/main/install.sh | sh

Нужен компилятор C++17 (g++ или clang++). Программа ставится в `~/.local/bin`. Если команда не находится, добавьте этот каталог в PATH.

Вручную:

    git clone https://github.com/deepsytxt/deepfetch
    cd deepfetch
    make
    sudo make install

## Использование

    deepfetch            запустить
    deepfetch --stop     остановить анимацию
    deepfetch --static   вывести один раз
    deepfetch --clear    остановить и очистить экран

Чтобы clear и Ctrl+L тоже убирали блок, добавьте в конфиг оболочки:

    fish   deepfetch --init fish | source
    bash   eval "$(deepfetch --init bash)"
    zsh    eval "$(deepfetch --init zsh)"

Если установлен fastfetch, логотип берётся из него. Иначе рисуется запасной.

## Удаление

    rm ~/.local/bin/deepfetch
