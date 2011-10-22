#include <bitcoin/storage/postgresql_storage.hpp>

#include <bitcoin/block.hpp>
#include <bitcoin/transaction.hpp>
#include <bitcoin/util/assert.hpp>
#include <bitcoin/util/logger.hpp>

#include "postgresql_blockchain.hpp"

namespace libbitcoin {

hash_digest hash_from_bytea(std::string byte_stream);
data_chunk bytes_from_bytea(std::string byte_stream);

postgresql_storage::postgresql_storage(std::string database, 
        std::string user, std::string password)
  : sql_(std::string("postgresql:dbname=") + database + 
        ";user=" + user + ";password=" + password)
{
    blockchain_.reset(new pq_blockchain(sql_, service()));
    // Organise/validate old blocks in case of unclean shutdown
    strand()->post(std::bind(&pq_blockchain::start, blockchain_));
}

void postgresql_storage::store(const message::inv& inv,
        store_handler handle_store)
{
    // temporarily disabled
    //strand()->post(std::bind(
    //    &postgresql_storage::do_store_inv, shared_from_this(), 
    //        inv, handle_store));
}
void postgresql_storage::do_store_inv(const message::inv& inv,
        store_handler handle_store)
{
    cppdb::statement stat = sql_ <<
        "INSERT INTO inventory_requests (type, hash) \
        VALUES (?, ?)";
    for (message::inv_vect ivv: inv.invs)
    {
        stat.reset();
        if (ivv.type == message::inv_type::transaction)
            stat.bind("transaction");
        else if (ivv.type == message::inv_type::block)
            stat.bind("block");
        std::string byte_stream = pretty_hex(ivv.hash);
        stat.bind(byte_stream);
        stat.exec();
    }
    handle_store(std::error_code());
}

void postgresql_storage::insert(const message::transaction_input& input,
        size_t transaction_id, size_t index_in_parent)
{
    std::string hash = pretty_hex(input.hash);
    sql_ <<
        "INSERT INTO inputs (transaction_id, index_in_parent, \
            script, previous_output_hash, previous_output_index, sequence) \
        VALUES (?, ?, decode(?, 'hex'), decode(?, 'hex'), ?, ?)"
        << transaction_id
        << index_in_parent
        << pretty_hex(save_script(input.input_script))
        << hash
        << input.index
        << input.sequence
        << cppdb::exec;
}

void postgresql_storage::insert(const message::transaction_output& output,
        size_t transaction_id, size_t index_in_parent)
{
    sql_ <<
        "INSERT INTO outputs ( \
            transaction_id, index_in_parent, script, value) \
        VALUES (?, ?, decode(?, 'hex'), internal_to_sql(?))"
        << transaction_id
        << index_in_parent
        << pretty_hex(save_script(output.output_script))
        << output.value
        << cppdb::exec;
}

size_t postgresql_storage::insert(const message::transaction& transaction)
{
    hash_digest transaction_hash = hash_transaction(transaction);
    std::string transaction_hash_repr = pretty_hex(transaction_hash);
    // We use special function to insert txs. 
    // Some blocks contain duplicates. See SQL for more details.
    cppdb::result result = sql_ <<
        "SELECT insert_transaction(decode(?, 'hex'), ?, ?, ?)"
        << transaction_hash_repr
        << transaction.version
        << transaction.locktime
        << is_coinbase(transaction)
        << cppdb::row;
    size_t transaction_id = result.get<size_t>(0);
    if (transaction_id == 0)
    {
        cppdb::result old_transaction_id = sql_ <<
            "SELECT transaction_id \
            FROM transactions \
            WHERE transaction_hash=decode(?, 'hex')"
            << transaction_hash_repr
            << cppdb::row;
        return old_transaction_id.get<size_t>(0);
    }

    for (size_t i = 0; i < transaction.inputs.size(); ++i)
        insert(transaction.inputs[i], transaction_id, i);
    for (size_t i = 0; i < transaction.outputs.size(); ++i)
        insert(transaction.outputs[i], transaction_id, i);
    return transaction_id;
}

void postgresql_storage::store(const message::transaction& transaction,
        store_handler handle_store)
{
    strand()->post(std::bind(
        &postgresql_storage::do_store_transaction, shared_from_this(),
            transaction, handle_store));
}
void postgresql_storage::do_store_transaction(
        const message::transaction& transaction, store_handler handle_store)
{
    insert(transaction);
    handle_store(std::error_code());
}

void postgresql_storage::store(const message::block& block,
        store_handler handle_store)
{
    strand()->post(std::bind(
        &postgresql_storage::do_store_block, shared_from_this(),
            block, handle_store));
}
void postgresql_storage::do_store_block(const message::block& block,
        store_handler handle_store)
{
    hash_digest block_hash = hash_block_header(block);
    std::string block_hash_repr = pretty_hex(block_hash),
            prev_block_repr = pretty_hex(block.prev_block),
            merkle_repr = pretty_hex(block.merkle_root);

    cppdb::transaction guard(sql_);
    cppdb::result result = sql_ <<
        "SELECT 1 FROM blocks WHERE block_hash=decode(?, 'hex')"
        << block_hash_repr << cppdb::row;
    if (!result.empty())
    {
        log_warning() << "Block '" << block_hash_repr << "' already exists";
        handle_store(error::object_already_exists);
        return;
    }

    static cppdb::statement statement = sql_.prepare(
        "INSERT INTO blocks( \
            block_id, \
            block_hash, \
            space, \
            depth, \
            span_left, \
            span_right, \
            version, \
            prev_block_hash, \
            merkle, \
            when_created, \
            bits_head, \
            bits_body, \
            nonce \
        ) VALUES ( \
            DEFAULT, \
            decode(?, 'hex'), \
            nextval('blocks_space_sequence'), \
            0, \
            0, \
            0, \
            ?, \
            decode(?, 'hex'), \
            decode(?, 'hex'), \
            TO_TIMESTAMP(?), \
            ?, \
            ?, \
            ? \
        ) \
        RETURNING block_id"
        );

    statement.reset();
    statement.bind(block_hash_repr);
    statement.bind(block.version);
    statement.bind(prev_block_repr);
    statement.bind(merkle_repr);
    statement.bind(block.timestamp);
    uint32_t bits_head = (block.bits >> (8*3)),
        bits_body = (block.bits & 0x00ffffff);
    statement.bind(bits_head);
    statement.bind(bits_body);
    statement.bind(block.nonce);

    result = statement.row();
    size_t block_id = result.get<size_t>(0);
    for (size_t i = 0; i < block.transactions.size(); ++i)
    {
        message::transaction transaction = block.transactions[i];
        size_t transaction_id = insert(transaction);
        // Create block <-> txn mapping
        sql_ << "INSERT INTO transactions_parents ( \
                    transaction_id, block_id, index_in_block) \
                VALUES (?, ?, ?)"
            << transaction_id << block_id << i << cppdb::exec;
    }
    // Finalise block
    sql_ << "UPDATE blocks SET status='orphan' WHERE block_id=?"
        << block_id << cppdb::exec;
    blockchain_->raise_barrier();
    guard.commit();
    handle_store(std::error_code());
}

void postgresql_storage::fetch_inventories(fetch_handler_inventories)
{
}
void postgresql_storage::do_fetch_inventories(fetch_handler_inventories)
{
    // Not implemented
}

void postgresql_storage::fetch_block_by_depth(size_t block_number,
        fetch_handler_block handle_fetch)
{
    strand()->post(std::bind(
        &postgresql_storage::do_fetch_block_by_depth, shared_from_this(),
            block_number, handle_fetch));
}
void postgresql_storage::do_fetch_block_by_depth(size_t block_number,
        fetch_handler_block handle_fetch)
{
    static cppdb::statement block_statement = sql_.prepare(
        "SELECT \
            *, \
            EXTRACT(EPOCH FROM when_created) timest \
        FROM blocks \
        WHERE \
            depth=? \
            AND span_left=0 \
            AND span_right=0"
        );
    block_statement.reset();
    block_statement.bind(block_number);
    cppdb::result block_result = block_statement.row();
    if (block_result.empty())
    {
        handle_fetch(error::object_doesnt_exist, message::block());
        return;
    }
    message::block block = std::get<1>(blockchain_->read_block(block_result));
    handle_fetch(std::error_code(), block);
}

void postgresql_storage::fetch_block_by_hash(hash_digest block_hash, 
        fetch_handler_block handle_fetch)
{
    strand()->post(std::bind(
        &postgresql_storage::do_fetch_block_by_hash, shared_from_this(),
            block_hash, handle_fetch));
}
void postgresql_storage::do_fetch_block_by_hash(hash_digest block_hash, 
        fetch_handler_block handle_fetch)
{
    static cppdb::statement block_statement = sql_.prepare(
        "SELECT \
            *, \
            EXTRACT(EPOCH FROM when_created) timest \
        FROM blocks \
        WHERE \
            block_hash=decode(?, 'hex') \
            AND span_left=0 \
            AND span_left=0"
        );
    std::string block_hash_repr = pretty_hex(block_hash);
    block_statement.reset();
    block_statement.bind(block_hash_repr);
    cppdb::result block_result = block_statement.row();
    if (block_result.empty())
    {
        handle_fetch(error::object_doesnt_exist, message::block());
        return;
    }
    message::block block = std::get<1>(blockchain_->read_block(block_result));
    handle_fetch(std::error_code(), block);
}

void postgresql_storage::fetch_block_locator(
        fetch_handler_block_locator handle_fetch)
{
    strand()->post(std::bind(
        &postgresql_storage::do_fetch_block_locator, shared_from_this(),
            handle_fetch));
}
void postgresql_storage::do_fetch_block_locator(
        fetch_handler_block_locator handle_fetch)
{
    cppdb::result number_blocks_result = sql_ <<
        "SELECT depth \
        FROM chains \
        ORDER BY work DESC \
        LIMIT 1"
        << cppdb::row;
    if (number_blocks_result.empty())
    {
        handle_fetch(error::object_doesnt_exist, message::block_locator());
        return;
    }
    // Start at max_depth
    int top_depth = number_blocks_result.get<size_t>(0);
    std::vector<size_t> indices;
    // Push last 10 indices first
    size_t step = 1, start = 0;
    for (int i = top_depth; i > 0; i -= step, ++start)
    {
        if (start >= 10)
            step *= 2;
        indices.push_back(i);
    }
    indices.push_back(0);
    // Now actually fetch the hashes for these blocks
    std::stringstream hack_sql;
    hack_sql <<
        "SELECT encode(block_hash, 'hex') \
        FROM main_chain \
        WHERE depth IN (";
    for (size_t i = 0; i < indices.size(); ++i)
    {
        if (i != 0)
            hack_sql << ", ";
        hack_sql << indices[i];
    }
    hack_sql << ") ORDER BY depth DESC";
    // ----------------------------------------------
    cppdb::result block_hashes_result = sql_ << hack_sql.str();
    message::block_locator locator;
    while (block_hashes_result.next())
    {
        std::string block_hash_repr = block_hashes_result.get<std::string>(0);
        locator.push_back(hash_from_bytea(block_hash_repr));
    }
    BITCOIN_ASSERT(locator.size() == indices.size());
    handle_fetch(std::error_code(), locator);
}

void postgresql_storage::fetch_output_by_hash(hash_digest transaction_hash, 
        uint32_t index, fetch_handler_output handle_fetch)
{
    strand()->post(std::bind(
        &postgresql_storage::do_fetch_output_by_hash, shared_from_this(),
            transaction_hash, index, handle_fetch));
}
void postgresql_storage::do_fetch_output_by_hash(hash_digest transaction_hash, 
        uint32_t index, fetch_handler_output handle_fetch)
{
    message::transaction_output output;
    std::string transaction_hash_repr = pretty_hex(transaction_hash);
    cppdb::result result = sql_ <<
        "SELECT \
            encode(script, 'hex') AS script, \
            sql_to_internal(value) AS internal_value \
        FROM \
            transactions, \
            outputs \
        WHERE \
            transaction_hash=decode(?, 'hex') \
            AND transactions.transaction_id=outputs.transaction_id \
            AND index_in_parent=?"
        << transaction_hash_repr
        << index
        << cppdb::row;
    if (result.empty())
    {
        handle_fetch(error::object_doesnt_exist, output);
        return;
    }
    output.value = result.get<uint64_t>("internal_value");
    output.output_script =
        parse_script(bytes_from_bytea(result.get<std::string>("script")));
    handle_fetch(std::error_code(), output);
}

void postgresql_storage::block_exists_by_hash(hash_digest block_hash,
        exists_handler handle_exists)
{
    strand()->post(std::bind(
        &postgresql_storage::do_block_exists_by_hash, shared_from_this(), 
            block_hash, handle_exists));
}
void postgresql_storage::do_block_exists_by_hash(hash_digest block_hash,
        exists_handler handle_exists)
{
    std::string block_hash_repr = pretty_hex(block_hash);
    cppdb::result block_result = sql_ <<
        "SELECT 1 \
        FROM blocks \
        WHERE \
            block_hash=decode(?, 'hex') \
            AND span_left=0 \
            AND span_left=0"
        << block_hash_repr
        << cppdb::row;
    if (block_result.empty())
        handle_exists(std::error_code(), false);
    else
        handle_exists(std::error_code(), true);
}

} // libbitcoin

