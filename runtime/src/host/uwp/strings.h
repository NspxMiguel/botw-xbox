// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <string_view>
namespace uwp {
enum class Text {
    title,
    play,
    resume,
    select,
    settings,
    mods,
    back,
    yes,
    no,
    auto_question,
    missing,
    error,
    retry,
    automatic,
    volume,
    fps,
    layout,
    positions,
    labels,
    mods_help,
    controls,
    restart,
    loading,
    count
};
inline std::wstring_view text(Text key, bool pt) {
    static constexpr std::array<std::array<std::wstring_view, 2>, static_cast<size_t>(Text::count)> table{
        {{L"BotW Recompiled", L"BotW Recompiled"},
         {L"Play", L"Jogar"},
         {L"Resume", L"Continuar"},
         {L"Select game folder", L"Selecionar pasta do jogo"},
         {L"Settings", L"Ajustes"},
         {L"Mods", L"Mods"},
         {L"Back", L"Voltar"},
         {L"Yes", L"Sim"},
         {L"No", L"Não"},
         {L"Next time, open the game automatically?", L"Na próxima vez, abrir o jogo automaticamente?"},
         {L"Copy your own updated Wii U game to LocalState/game (code, content, meta), or select its folder.",
          L"Copie seu jogo Wii U atualizado para LocalState/game (code, content, meta) ou selecione a "
          L"pasta."},
         {L"Unable to open the game. Check the folder and storage permissions, then try again.",
          L"Não foi possível abrir o jogo. Confira a pasta e as permissões de armazenamento e tente "
          L"novamente."},
         {L"Scan again", L"Buscar novamente"},
         {L"Open automatically", L"Abrir automaticamente"},
         {L"Volume", L"Volume"},
         {L"Frame rate", L"Taxa de quadros"},
         {L"Button layout", L"Posição dos botões"},
         {L"Nintendo positions", L"Posições Nintendo"},
         {L"Button labels", L"Letras dos botões"},
         {L"Place mods in this folder. Changes take effect next launch.",
          L"Coloque os mods nesta pasta. As mudanças valem na próxima abertura."},
         {L"View + Menu: launcher. View + Y: GamePad screen. Xbox has no touch or gyro.",
          L"View + Menu: menu inicial. View + Y: tela do GamePad. Xbox não tem toque nem giroscópio."},
         {L"Check your updated game folder, then close and reopen the app to try again.",
          L"Confira a pasta do jogo atualizado, feche e abra o app para tentar novamente."},
         {L"Loading your game…", L"Carregando seu jogo…"}}};
    return table.at(static_cast<size_t>(key))[pt ? 1 : 0];
}
} // namespace uwp
