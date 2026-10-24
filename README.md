# deepfetch

Системная информация в терминале с живыми данными (CPU, RAM, GPU, диски, сеть) и вращающимся 3D ASCII-логотипом. Запускается как fastfetch и работает в фоне, терминал остаётся свободным.

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

Чтобы clear и Ctrl+L корректно убирали блок, нужна интеграция с оболочкой (fish, bash, zsh). Установщик добавляет её сам, при ручной сборке программа предложит это при первом запуске. Позже:

    deepfetch --setup      добавить интеграцию
    deepfetch --unsetup    убрать

Если окно терминала слишком маленькое, блок сжимается, а если совсем не помещается, выводится статичный вариант с пояснением.

Если установлен fastfetch, логотип берётся из него. Иначе рисуется запасной.

## Удаление

    deepfetch --unsetup
    rm ~/.local/bin/deepfetch
