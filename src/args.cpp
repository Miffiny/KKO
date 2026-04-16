#include "args.hpp"
#include <argparse.hpp>
#include <iostream>
#include <stdexcept>

ParsedArgs parse_arguments(int argc, char** argv) {
    argparse::ArgumentParser program("lz_codec");

    program.add_argument("-c")
        .help("Komprese obrazových dat")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("-d")
        .help("Dekomprese zkomprimovaných dat")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("-m")
        .help("Aktivuje model pro předzpracování dat")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("-a")
        .help("Aktivuje adaptivní skenování obrazu")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("-i")
        .help("Vstupní soubor")
        .required();

    program.add_argument("-o")
        .help("Výstupní soubor")
        .required();

    program.add_argument("-w")
        .help("Šířka obrazu (pouze pro kompresi)")
        .scan<'i', int>();

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        std::cerr << err.what() << "\n";
        std::cerr << program;
        exit(1);
    }

    ParsedArgs args;

    bool compress = program.get<bool>("-c");
    bool decompress = program.get<bool>("-d");

    if (compress == decompress) {
        std::cerr << "Musíte zadat právě jeden režim: -c nebo -d\n";
        exit(1);
    }

    args.decompress = decompress;
    args.infile = program.get<std::string>("-i");
    args.outfile = program.get<std::string>("-o");
    args.use_model = program.get<bool>("-m");
    args.adaptive_scan = program.get<bool>("-a");

    if (compress) {
        if (!program.is_used("-w")) {
            std::cerr << "Při kompresi musíte zadat šířku obrazu pomocí -w\n";
            exit(1);
        }
        args.width = program.get<int>("-w");
        if (args.width < 1) {
            std::cerr << "Šířka obrazu musí být >= 1\n";
            exit(1);
        }
        if (args.width % 256 != 0) {
            std::cerr << "Šířka obrazu musí být násobkem 256\n";
            exit(1);
        }
    }

    if (decompress && program.is_used("-w")) {
        std::cerr << "Neočekávaný parametr -w\n";
        exit(1);
    }

    if (decompress && program.is_used("-m")) {
        std::cerr << "Neočekávaný parametr -m\n";
        exit(1);
    }

    if (decompress && program.is_used("-a")) {
        std::cerr << "Neočekávaný parametr -a\n";
        exit(1);
    }

    return args;
}
