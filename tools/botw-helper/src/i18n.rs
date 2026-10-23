pub fn text(key: &str) -> &'static str {
    let portuguese =
        sys_locale::get_locale().is_some_and(|locale| locale.to_lowercase().starts_with("pt"));
    let pair = match key {
        "intro" => ("Seu launcher, do seu jeito.", "Make the launcher yours."),
        "connection" => ("Xbox em Dev Mode", "Xbox in Dev Mode"),
        "host" => ("Endereço do Xbox (IP:porta)", "Xbox address (IP:port)"),
        "connection_hint" => (
            "Na mesma rede, copie o endereço de Dev Home › Remote Access.",
            "On the same network, copy the address from Dev Home › Remote Access.",
        ),
        "user" => ("Usuário do Portal", "Portal username"),
        "password" => ("Senha do Portal", "Portal password"),
        "remember" => (
            "Lembrar senha neste computador",
            "Remember password on this computer",
        ),
        "preferences" => ("Launcher", "Launcher"),
        "auto_launch" => (
            "Iniciar o jogo automaticamente",
            "Start the game automatically",
        ),
        "setup_hint" => (
            "Conclua a configuração no Xbox antes de usar o início automático.",
            "Complete setup on the Xbox before using automatic start.",
        ),
        "fps" => ("Quadros por segundo", "Frames per second"),
        "volume" => ("Volume", "Volume"),
        "buttons" => ("Rótulos dos botões", "Button labels"),
        "wallpaper" => ("Papel de parede", "Wallpaper"),
        "default" => ("Padrão", "Default"),
        "render_scale" => (
            "Escala de renderização (%) · 0 = automático",
            "Render scale (%) · 0 = automatic",
        ),
        "upscaler" => ("Upscaler (0–3)", "Upscaler (0–3)"),
        "fsr_quality" => ("Qualidade FSR (0–4)", "FSR quality (0–4)"),
        "frame_generation" => ("Geração de quadros (0–2)", "Frame generation (0–2)"),
        "more" => ("Imagem e desempenho", "Picture and performance"),
        "cover" => ("Capa no Xbox", "Xbox cover"),
        "cover_hint" => (
            "Escolha uma imagem ou a pasta do seu próprio jogo Wii U para criar a capa.",
            "Choose an image or your own Wii U game folder to create the cover.",
        ),
        "repack_cover" => (
            "Aplicar capa no painel Xbox…",
            "Apply cover to Xbox dashboard…",
        ),
        "repack_hint" => (
            "Escolha a capa e o pacote .appx. O novo pacote será assinado e instalado pela conexão salva do nxbox, se o instalador estiver disponível. Os saves são preservados.",
            "Choose artwork and an .appx package. The new package is signed and installed using the saved nxbox connection when its installer is available. Saves are preserved.",
        ),
        "repacked" => (
            "Pacote pronto. Instale pelo Device Portal:",
            "Package ready. Install through Device Portal:",
        ),
        "repacked_installed" => (
            "Capa instalada no painel Xbox. Pacote salvo em:",
            "Xbox dashboard cover installed. Package saved at:",
        ),
        "repack_failed" => (
            "Confira o pacote, os logos e o certificado de assinatura e tente novamente.",
            "Check the package, logos and signing certificate, then try again.",
        ),
        "install_failed" => (
            "O pacote está pronto, mas a instalação falhou. Confira a conexão salva do nxbox ou instale pelo Portal:",
            "The package is ready, but installation failed. Check the saved nxbox connection or install through Portal:",
        ),
        "choose_image" => ("Escolher PNG ou JPG", "Choose PNG or JPG"),
        "own_game" => ("Do meu próprio jogo", "From my own game"),
        "keep_default" => ("Manter padrão", "Keep default"),
        "default_hint" => (
            "A capa não será alterada. Para usar o padrão original, remova as capas anteriores de LocalState.",
            "The cover will stay unchanged. To restore the original default, remove previous covers from LocalState.",
        ),
        "send" => ("Enviar ao Xbox", "Send to Xbox"),
        "launch" => ("Abrir aplicativo", "Launch app"),
        "busy" => ("Aguarde…", "Please wait…"),
        "sent" => (
            "Enviado. As preferências e a capa serão aplicadas na próxima abertura do aplicativo.",
            "Sent. Preferences and cover will apply the next time the app opens.",
        ),
        "launched" => ("Abertura solicitada.", "Launch requested."),
        "failed" => (
            "Não foi possível concluir. Confira o endereço, as credenciais e tente novamente.",
            "Could not complete. Check the address and credentials, then try again.",
        ),
        "cover_failed" => (
            "Não foi possível ler a capa. Escolha outra imagem ou a pasta que contém meta/.",
            "Could not read the cover. Choose another image or the folder containing meta/.",
        ),
        "save_failed" => (
            "Não foi possível salvar a conexão neste computador.",
            "Could not save the connection on this computer.",
        ),
        _ => ("", ""),
    };
    if portuguese { pair.0 } else { pair.1 }
}
