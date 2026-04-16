#include "args.hpp"
#include "codec.hpp"
#include <iostream>
#include <exception>

int main(int argc, char *argv[]) {
    try {
        ParsedArgs args = parse_arguments(argc, argv);

#ifdef DEBUG
        std::cout << "Režim: " << (args.decompress ? "dekomprese" : "komprese") << "\n";
        std::cout << "Vstupní soubor: " << args.infile << "\n";
        std::cout << "Výstupní soubor: " << args.outfile << "\n";

        if (!args.decompress) {
            std::cout << "Šířka obrazu: " << args.width << "\n";
            std::cout << "Model: " << (args.use_model ? "aktivní" : "neaktivní") << "\n";
            std::cout << "Skenování: " << (args.adaptive_scan ? "adaptivní" : "sekvenční") << "\n";
        }
#endif

        if (args.decompress) {
            decompress_file(args);
        } else {
            compress_file(args);
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}