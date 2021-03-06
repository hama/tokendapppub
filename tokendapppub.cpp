#include "tokendapppub.hpp"

void tokendapppub::create(account_name issuer, asset maximum_supply) {
    require_auth(issuer);
    
    auto sym = maximum_supply.symbol;
    eosio_assert( sym.is_valid(), "invalid symbol name" );
    eosio_assert( maximum_supply.is_valid(), "invalid supply");
    eosio_assert( maximum_supply.amount > 0, "max-supply must be positive");

    tb_games game_sgt(_self, sym.name());
    eosio_assert(game_sgt.exists(), "game not found by this symbol name");
    st_game game = game_sgt.get();
    eosio_assert(game.owner == issuer, "issuer is not the owner of this token");
    eosio_assert(game.base_stake - game.deserved_option + game.base_option == maximum_supply.amount, "invalid maximum supply");

    stats statstable(_self, sym.name());
    auto existing = statstable.find(sym.name());
    eosio_assert(existing == statstable.end(), "token with symbol already exists" );

    statstable.emplace(issuer, [&]( auto& s) {
        s.supply.symbol = maximum_supply.symbol;
        s.max_supply = maximum_supply;
        s.issuer = issuer;
    });
}

void tokendapppub::issue(account_name to, asset quantity, string memo) {
    auto sym = quantity.symbol;
    eosio_assert( sym.is_valid(), "invalid symbol name" );
    eosio_assert( memo.size() <= 256, "memo has more than 256 bytes" );

    auto sym_name = sym.name();
    stats statstable( _self, sym_name );
    auto existing = statstable.find( sym_name );
    eosio_assert( existing != statstable.end(), "token with symbol does not exist, create token before issue" );
    const auto& st = *existing;

    require_auth( st.issuer );
    eosio_assert( quantity.is_valid(), "invalid quantity" );
    eosio_assert( quantity.amount > 0, "must issue positive quantity" );

    eosio_assert( quantity.symbol == st.supply.symbol, "symbol precision mismatch" );
    eosio_assert( quantity.amount <= st.max_supply.amount - st.supply.amount, "quantity exceeds available supply");

    statstable.modify( st, 0, [&]( auto& s ) {
       s.supply += quantity;
    });
}

void tokendapppub::reg(account_name from, string memo) {
    require_auth(from);
    eosio_assert(memo.length() <= 7, "invalid memo format");
    symbol_name name = string_to_symbol(0, memo.c_str()) >> 8;
    
    tb_games game_sgt(_self, name);
    eosio_assert(game_sgt.exists(), "token not found by this symbol name");

    accounts from_player(_self, from);
    auto player_itr = from_player.find(name);
    if (player_itr == from_player.end()) {
        from_player.emplace(from, [&](auto& rt){
            rt.balance = asset(0, game_sgt.get().symbol);
        });
    }
}

void tokendapppub::buy(account_name from, account_name to, asset quantity, string memo) {
    if (from == _self || to != _self) {
        return;
    }
    eosio_assert(quantity.symbol == CORE_SYMBOL, "must pay with CORE token");

    if (memo.find('-') != string::npos) {
        auto first_separator_pos = memo.find('-');
        eosio_assert(first_separator_pos != string::npos, "invalid memo format");

        string name_str = memo.substr(0, first_separator_pos);
        eosio_assert(name_str.length() <= 7, "invalid symbol name");
        symbol_name name = string_to_symbol(0, name_str.c_str()) >> 8;

        string profit_str = memo.substr(first_separator_pos+1);
        eosio_assert(profit_str == "profit", "invalid memo format for profit");

        game_profit(name, quantity.amount);
        return;
    }

    eosio_assert(memo.length() <= 7, "invalid memo format");
    symbol_name name = string_to_symbol(0, memo.c_str()) >> 8;

    asset stake_quantity = game_buy(name, quantity.amount);

    accounts from_player(_self, from);
    auto player_itr = from_player.find(name);
    if (player_itr == from_player.end()) {
        from_player.emplace(from, [&](auto& rt){
            rt.balance = stake_quantity;
        });
    } else {
        from_player.modify(player_itr, from, [&](auto& rt){
            rt.balance += stake_quantity;
        });
    }

    action(
            permission_level{_self, N(active)},
            _self,
            N(receipt),
            make_tuple(from, string("buy"), quantity, stake_quantity, asset())
    ).send();
}

void tokendapppub::sell(account_name from, asset quantity) {
    require_auth(from);
    accounts from_player(_self, from);
    auto player_itr = from_player.find(quantity.symbol.name());
    eosio_assert(player_itr != from_player.end(), "account not found");
    eosio_assert(quantity.symbol == player_itr->balance.symbol, "symbol precision mismatch");
    eosio_assert((quantity.amount > 0) && (quantity.amount <= player_itr->balance.amount), "invalid amount");

    asset eos_quantity, all_quantity;
    tie(eos_quantity, all_quantity) = game_sell(quantity.symbol.name(), quantity.amount);
    eosio_assert(eos_quantity.amount > 0, "selled eos amount should be greater than 0");

    action(
            permission_level{_self, N(active)},
            N(eosio.token),
            N(transfer),
            make_tuple(_self, from, eos_quantity, string("tokendapppub withdraw https://dapp.pub"))
    ).send();

    from_player.modify(player_itr, from, [&](auto& rt){
        rt.balance -= quantity;
    });

    if (player_itr->balance.amount == 0) {
        from_player.erase(player_itr);
    }

    action(
            permission_level{_self, N(active)},
            _self,
            N(receipt),
            make_tuple(from, string("sell"), quantity, all_quantity, all_quantity-eos_quantity)
    ).send();
}

void tokendapppub::consume(account_name from, asset quantity, string memo) {
    require_auth(from);
    accounts from_player(_self, from);
    auto player_itr = from_player.find(quantity.symbol.name());
    eosio_assert(player_itr != from_player.end(), "player not found");
    eosio_assert((quantity.amount > 0) && (quantity.amount <= player_itr->balance.amount), "not enough balance to consume");
    eosio_assert(quantity.symbol == player_itr->balance.symbol, "symbol precision mismatch");

    game_consume(quantity.symbol.name(), quantity.amount);

    from_player.modify(player_itr, from, [&](auto& rt){
        rt.balance -= quantity;
    });

    if (player_itr->balance.amount == 0) {
        from_player.erase(player_itr);
    }
}

void tokendapppub::claim(string name_str, bool sell) {
    symbol_name name = _string_to_symbol_name(name_str.c_str());
    tb_games game_sgt(_self, name);
    eosio_assert(game_sgt.exists(), "token not found by this symbol_name");
    st_game game = game_sgt.get();
    require_auth(game.owner);

    asset stake_quantity = game_claim(name);

    accounts from_player(_self, game.owner);
    auto player_itr = from_player.find(name);
    if (player_itr == from_player.end()) {
        from_player.emplace(game.owner, [&](auto& rt){
            rt.balance = stake_quantity;
        });
    } else {
        from_player.modify(player_itr, game.owner, [&](auto& rt){
            rt.balance += stake_quantity;
        });
    }

    if (sell) {
        player_itr = from_player.find(name);
        eosio_assert(player_itr != from_player.end(), "WTF!");
        this->sell(game.owner, player_itr->balance);
    }
}

void tokendapppub::transfer(account_name from, account_name to, asset quantity, string memo) {
    eosio_assert(from != to, "cannot transfer to self");
    require_auth(from);
    eosio_assert(is_account(to), "to account does not exist");
    auto sym = quantity.symbol.name();
    
    tb_games game_sgt(_self, sym);
    eosio_assert(game_sgt.exists(), "game not found by this symbol name");
    st_game game = game_sgt.get();
    eosio_assert(quantity.is_valid(), "invalid quantity");
    eosio_assert(quantity.amount > 0, "must transfer positive quantity");
    eosio_assert(quantity.symbol == game.symbol, "symbol precision mismatch");
    eosio_assert(memo.size() <= 256, "memo has more than 256 bytes");

    require_recipient( from );
    require_recipient( to );

    accounts from_player(_self, from);
    auto player_itr = from_player.find(quantity.symbol.name());
    eosio_assert(player_itr != from_player.end(), "no balance object found by from account");
    eosio_assert(player_itr->balance.amount >= quantity.amount, "overdrawn balance" );
    from_player.modify(player_itr, from, [&](auto& rt){
        rt.balance -= quantity;
    });

    if (player_itr->balance.amount == 0) {
        from_player.erase(player_itr);
    }

    accounts to_player(_self, to);
    auto to_player_itr = to_player.find(quantity.symbol.name());
    if (to_player_itr == to_player.end()) {
        to_player.emplace(from, [&](auto& rt){
            rt.balance = quantity;
        });
    } else {
        to_player.modify(to_player_itr, from, [&](auto& rt){
            rt.balance += quantity;
        });
    }
}

void tokendapppub::destroy(string name_str) {
    tb_games game_sgt(_self, _string_to_symbol_name(name_str.c_str()));
    eosio_assert(game_sgt.exists(), "token not found by this symbol_name");
    st_game game = game_sgt.get();
    require_auth(game.owner);

    eosio_assert(game.base_stake == game.stake, "all stake should be retrieved before erasing game");
    game_sgt.remove();

    stats statstable(_self, _string_to_symbol_name(name_str.c_str()));
    auto existing = statstable.find(_string_to_symbol_name(name_str.c_str()));
    eosio_assert(existing != statstable.end(), "token with symbol do not exists" );
    statstable.erase(existing);
}

void tokendapppub::hellodapppub(asset base_eos_quantity, asset maximum_stake, asset option_quantity,
                                uint32_t lock_up_period,
                                uint8_t base_fee_percent, uint8_t init_fee_percent) {
    require_auth(GOD_ACCOUNT);
    new_game(GOD_ACCOUNT, base_eos_quantity, maximum_stake, option_quantity, lock_up_period, base_fee_percent, init_fee_percent);

    SEND_INLINE_ACTION(*this, create, {GOD_ACCOUNT, N(active)}, {GOD_ACCOUNT, maximum_stake});
    SEND_INLINE_ACTION(*this, issue, {GOD_ACCOUNT, N(active)}, {GOD_ACCOUNT, maximum_stake, string("")});
}

void tokendapppub::newtoken(account_name from, asset base_eos_quantity, asset maximum_stake, asset option_quantity,
                            uint32_t lock_up_period,
                            uint8_t base_fee_percent, uint8_t init_fee_percent) {
    require_auth(from);
    eosio_assert(maximum_stake.symbol.name_length() >= 5, "the length of token name should be greater than five");
    this->consume(from, NEW_GAME_CONSOME, "consume for new token");
    new_game(from, base_eos_quantity, maximum_stake, option_quantity, lock_up_period, base_fee_percent, init_fee_percent);

    SEND_INLINE_ACTION(*this, create, {from, N(active)}, {from, maximum_stake});
    SEND_INLINE_ACTION(*this, issue, {from, N(active)}, {from, maximum_stake, string("")});
}

void tokendapppub::receipt(account_name from, string type, asset in, asset out, asset fee) {
    require_auth(_self);
}

extern "C" {
    void apply( uint64_t receiver, uint64_t code, uint64_t action ) {
        tokendapppub thiscontract(receiver);

        if((code == N(eosio.token)) && (action == N(transfer))) {
            execute_action(&thiscontract, &tokendapppub::buy);
            return;
        }

        if (code != receiver) return;

        switch (action) {
            EOSIO_API(tokendapppub, (issue)(create)(reg)(receipt)(transfer)(sell)(consume)(destroy)(claim)(newtoken)(hellodapppub))
        };
        eosio_exit(0);
    }
}