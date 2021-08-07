#include "archive.hpp"
#include "error.hpp"
#include "offline_dfa.hpp"
#include "online_dfa.hpp"

#include <chrono>
#include <iostream>
#include <optional>

#include <CLI/CLI.hpp>

template <class Func>
std::chrono::microseconds timeit(Func f)
{
    auto begin = std::chrono::high_resolution_clock::now();
    f();
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(end - begin);
}

template <class T1, class T2>
void print(T1 key, T2 value)
{
    std::cout << key << "," << value << "\n";
}

template <class Key, class Func>
void print_elapsed(Key key, Func func)
{
    auto elapsed = timeit(func);
    print(key, elapsed.count());
}

template <class Func>
void each_input_bit(const std::string& input_filename, size_t num_ap, Func func)
{
    std::ifstream ifs{input_filename};
    assert(ifs);

    while (ifs) {
        int ch = ifs.get();
        if (ch == EOF)
            break;
        uint8_t v = ch;
        for (size_t i = 0; i < num_ap; i++) {
            bool b = (v & 1u) != 0;
            v >>= 1;
            func(b);
        }
    }
}

template <class Runner>
void enc_run_dec_loop(const SecretKey& skey, const BKey& bkey,
                      const std::string& input_filename, size_t num_ap,
                      Runner runner)
{
    each_input_bit(input_filename, num_ap, [&](bool input) {
        // Encrypt
        TRGSWLvl1FFT enc_input;
        print_elapsed("enc", [&] {
            enc_input = encrypt_bit_to_TRGSWLvl1FFT(input, skey);
        });

        // Run
        bool output_exists = false;
        print_elapsed("run", [&] { output_exists = runner.run(enc_input); });

        // Decrypt if output exists
        if (output_exists) {
            bool result = false;
            print_elapsed("dec", [&] {
                result = decrypt_TLWELvl1_to_bit(runner.result(), skey);
            });
            print("result", result);
        }
    });
}

class OnlineDFA2BenchRunner {
private:
    OnlineDFARunner2 runner_;
    size_t output_freq_, num_processed_;
    TLWELvl1 result_;

public:
    OnlineDFA2BenchRunner(const std::string& spec_filename, size_t output_freq,
                          size_t bootstrapping_freq, const BKey& bkey)
        : runner_(Graph::from_file(spec_filename), bootstrapping_freq,
                  bkey.gkey),
          output_freq_(output_freq),
          num_processed_(0)
    {
    }

    bool run(const TRGSWLvl1FFT& input)
    {
        runner_.eval_one(input);
        num_processed_++;
        if (num_processed_ % output_freq_ != 0)
            return false;
        result_ = runner_.result();
        return true;
    }

    TLWELvl1 result() const
    {
        return result_;
    }
};

class OnlineDFA3BenchRunner {
private:
    OnlineDFARunner3 runner_;
    size_t output_freq_, queue_size_, bootstrapping_freq_, num_processed_;
    TLWELvl1 result_;

public:
    OnlineDFA3BenchRunner(const std::string& spec_filename, size_t output_freq,
                          size_t queue_size, size_t bootstrapping_freq,
                          const BKey& bkey)
        : runner_(Graph::from_file(spec_filename), queue_size,
                  bootstrapping_freq, *bkey.gkey, *bkey.tlwel1_trlwel1_ikskey,
                  std::nullopt),
          output_freq_(output_freq),
          queue_size_(queue_size),
          bootstrapping_freq_(bootstrapping_freq),
          num_processed_(0)
    {
    }

    bool run(const TRGSWLvl1FFT& input)
    {
        runner_.eval_one(input);
        num_processed_++;
        if (num_processed_ % output_freq_ != 0)
            return false;
        result_ = runner_.result();
        return true;
    }

    TLWELvl1 result() const
    {
        return result_;
    }
};

void do_reversed(const std::string& spec_filename,
                 const std::string& input_filename, size_t output_freq,
                 size_t bootstrapping_freq, size_t num_ap)
{
    print("config-spec", spec_filename);
    print("config-input", input_filename);
    print("config-output_freq", output_freq);
    print("config-bootstrapping_freq", bootstrapping_freq);
    print("config-num_ap", num_ap);

    std::optional<SecretKey> skey_opt;
    std::optional<BKey> bkey_opt;

    auto skey_elapsed = timeit([&] { skey_opt.emplace(); });
    const SecretKey& skey = skey_opt.value();
    auto bkey_elapsed = timeit([&] { bkey_opt.emplace(skey); });
    const BKey& bkey = bkey_opt.value();

    print("skey", skey_elapsed.count());
    print("bkey", bkey_elapsed.count());

    OnlineDFA2BenchRunner runner{spec_filename, output_freq, bootstrapping_freq,
                                 bkey};
    enc_run_dec_loop(skey, bkey, input_filename, num_ap, runner);
}

void do_qtrlwe2(const std::string& spec_filename,
                const std::string& input_filename, size_t output_freq,
                size_t queue_size, size_t bootstrapping_freq, size_t num_ap)
{
    print("config-spec", spec_filename);
    print("config-input", input_filename);
    print("config-output_freq", output_freq);
    print("config-queue_size", queue_size);
    print("config-bootstrapping_freq", bootstrapping_freq);
    print("config-num_ap", num_ap);

    std::optional<SecretKey> skey_opt;
    std::optional<BKey> bkey_opt;

    auto skey_elapsed = timeit([&] { skey_opt.emplace(); });
    const SecretKey& skey = skey_opt.value();
    auto bkey_elapsed = timeit([&] { bkey_opt.emplace(skey); });
    const BKey& bkey = bkey_opt.value();

    print("skey", skey_elapsed.count());
    print("bkey", bkey_elapsed.count());

    OnlineDFA3BenchRunner runner{spec_filename, output_freq, queue_size,
                                 bootstrapping_freq, bkey};
    enc_run_dec_loop(skey, bkey, input_filename, num_ap, runner);
}

int main(int argc, char** argv)
{
    CLI::App app{"Benchmark runner"};
    app.require_subcommand();

    enum class TYPE {
        REVERSED,
        QTRLWE2,
    } type;
    std::string spec_filename, input_filename;
    size_t output_freq, num_ap, queue_size, bootstrapping_freq;

    {
        CLI::App* rev = app.add_subcommand("reversed", "Run online-reversed");
        rev->parse_complete_callback([&] { type = TYPE::REVERSED; });
        rev->add_option("--ap", num_ap)->required()->check(CLI::PositiveNumber);
        rev->add_option("--out-freq", output_freq)
            ->required()
            ->check(CLI::PositiveNumber);
        rev->add_option("--bootstrapping-freq", bootstrapping_freq)
            ->required()
            ->check(CLI::PositiveNumber);
        rev->add_option("--spec", spec_filename)
            ->required()
            ->check(CLI::ExistingFile);
        rev->add_option("--in", input_filename)
            ->required()
            ->check(CLI::ExistingFile);
    }
    {
        CLI::App* qtrlwe2 = app.add_subcommand("qtrlwe2", "Run online-qtrlwe2");
        qtrlwe2->parse_complete_callback([&] { type = TYPE::QTRLWE2; });
        qtrlwe2->add_option("--ap", num_ap)
            ->required()
            ->check(CLI::PositiveNumber);
        qtrlwe2->add_option("--out-freq", output_freq)
            ->required()
            ->check(CLI::PositiveNumber);
        qtrlwe2->add_option("--queue-size", queue_size)
            ->required()
            ->check(CLI::PositiveNumber);
        qtrlwe2->add_option("--bootstrapping-freq", bootstrapping_freq)
            ->required()
            ->check(CLI::PositiveNumber);
        qtrlwe2->add_option("--spec", spec_filename)
            ->required()
            ->check(CLI::ExistingFile);
        qtrlwe2->add_option("--in", input_filename)
            ->required()
            ->check(CLI::ExistingFile);
    }

    CLI11_PARSE(app, argc, argv);

    switch (type) {
    case TYPE::REVERSED:
        do_reversed(spec_filename, input_filename, output_freq,
                    bootstrapping_freq, num_ap);
        break;
    case TYPE::QTRLWE2:
        do_qtrlwe2(spec_filename, input_filename, output_freq, queue_size,
                   bootstrapping_freq, num_ap);
        break;
    }

    return 0;
}
