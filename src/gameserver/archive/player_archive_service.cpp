#include "player_archive_service.h"

#include <exception>
#include <utility>

#include "MongoError.h"
#include "PlayerMongoStorage.h"
#include "ecs/components/account_component.h"
#include "ecs/entity/entity.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/event_loop.h"

// 业务↔DB 粘合层：搬迁自 game_server.cpp:572-738 三段实现。
// 与原实现行为等价，唯一差异：
//   - weak_from_this() 改为指向 PlayerArchiveService 自身（替代原指向 GameServer）。
//   - self->RunInLoop 改为 self->loop_.RunInLoop（loop_ 是 GameServer::loop_ 的引用）。
//   - storage_ 替代 player_storage_。
// 内存安全保证见头文件注释。

void PlayerArchiveService::PostAccountInfoAfterLogin(const EntityPtr& entity)
{
    if (!storage_ || !entity)
    {
        return;
    }

    const auto* account = entity->GetComponent<AccountComponent>();
    if (!account)
    {
        LOG_WARN << "account_info skipped: no AccountComponent, entity="
                 << entity->GetId();
        return;
    }
    try
    {
        if (!mongo::IsValidAccountInfoAccount(account->uid_))
        {
            LOG_WARN << "account_info skipped: account must be valid UTF-8 with 1-20 characters"
                     << " account_bytes=" << account->uid_.size()
                     << " channel_id=" << account->channel_id_;
            return;
        }

        mongo::AccountInfoSnapshot snapshot;
        snapshot.account = account->uid_;
        snapshot.channelId = account->channel_id_;
        // 传入 AccountComponent 已有的 account_id（重连场景）或 0（首次登录，mongo 层自动生成）。
        // 若命中已有文档（同一账号重复登录），worker 会 FindOne 读出实际 account_id 回调返回，
        // 覆盖此处可能新生成的值，保证 AccountComponent.account_id_ 与 mongo 一致。
        snapshot.accountId = account->account_id_;
        const auto accountBytes = snapshot.account.size();
        const auto channelId = snapshot.channelId;

        // 弱引用 entity：回调在 worker 线程执行，需投回业务线程后 lock entity
        std::weak_ptr<Entity> weak_entity(entity);
        storage_->PostUpsertAccountInfo(
            std::move(snapshot),
            [weak = weak_from_this(), weak_entity, accountBytes, channelId]
            (bool success, std::int64_t account_id, std::int64_t documentId, std::exception_ptr ep)
            {
                auto self = weak.lock();
                if (!self)
                {
                    return;
                }
                // 投回业务线程：entity 引用和 AccountComponent 写入必须在业务线程执行
                self->loop_.RunInLoop(
                    [weak_entity, accountBytes, channelId, success, account_id, documentId, ep]()
                    {
                        auto ent = weak_entity.lock();
                        if (!ent)
                        {
                            return;
                        }

                        if (success)
                        {
                            // 回写 AccountComponent.account_id_，供后续 PostSave 落地 players 顶层字段
                            if (account_id != 0)
                            {
                                auto* acc = ent->GetComponent<AccountComponent>();
                                if (acc)
                                {
                                    acc->account_id_ = account_id;
                                }
                            }
                            LOG_INFO << "account_info login: account_bytes=" << accountBytes
                                     << " channel_id=" << channelId
                                     << " account_id=" << account_id
                                     << " documentId=" << documentId;
                            return;
                        }

                        // 失败日志
                        if (!ep)
                        {
                            LOG_WARN << "account_info upsert rejected: queue full or stopping"
                                     << " account_bytes=" << accountBytes
                                     << " channel_id=" << channelId
                                     << " area_id=0";
                            return;
                        }
                        try
                        {
                            std::rethrow_exception(ep);
                        }
                        catch (const mongo::MongoError& error)
                        {
                            LOG_WARN << "account_info upsert failed"
                                     << " account_bytes=" << accountBytes
                                     << " channel_id=" << channelId
                                     << " area_id=0"
                                     << " op=" << error.operation()
                                     << " code=" << error.code()
                                     << " what=" << error.what();
                        }
                        catch (const std::exception& error)
                        {
                            LOG_WARN << "account_info upsert failed"
                                     << " account_bytes=" << accountBytes
                                     << " channel_id=" << channelId
                                     << " area_id=0"
                                     << " what=" << error.what();
                        }
                    });
            });
    }
    catch (const std::exception& error)
    {
        LOG_WARN << "account_info submit failed before enqueue"
                 << " account_bytes=" << account->uid_.size()
                 << " channel_id=" << account->channel_id_
                 << " area_id=0"
                 << " what=" << error.what();
    }
    catch (...)
    {
        LOG_WARN << "account_info submit failed before enqueue"
                 << " account_bytes=" << account->uid_.size()
                 << " channel_id=" << account->channel_id_
                 << " area_id=0 unknown_error";
    }
}

void PlayerArchiveService::QueryRole(uint64_t roleId, std::function<void(bool, ::entity_player_data)> cb)
{
    if (!storage_)
    {
        loop_.RunInLoop([cb = std::move(cb)]()
        {
            cb(false, {});
        });
        return;
    }
    storage_->PostLoad(static_cast<std::int64_t>(roleId),
        [weak = weak_from_this(), roleId, cb = std::move(cb)](bool success, std::int64_t,
                                                                ::entity_player_data data,
                                                                std::exception_ptr ep)
        {
            auto self = weak.lock();
            if (!self)
            {
                // UAF 防护：facade 已析构（GameServer 关服），丢弃。
                return;
            }
            self->loop_.RunInLoop([cb = std::move(cb), success, data = std::move(data), ep, roleId]()
            {
                if (ep)
                {
                    try
                    {
                        std::rethrow_exception(ep);
                    }
                    catch (const std::exception& e)
                    {
                        LOG_WARN << "QueryRole failed role_id=" << roleId << " what=" << e.what();
                    }
                }
                cb(success, std::move(data));
            });
        });
}

void PlayerArchiveService::DeleteRoleArchive(uint64_t roleId, std::function<void(bool)> cb)
{
    if (!storage_)
    {
        if (cb)
        {
            loop_.RunInLoop([cb = std::move(cb)]()
            {
                cb(false);
            });
        }
        return;
    }
    storage_->PostDelete(static_cast<std::int64_t>(roleId),
        [weak = weak_from_this(), roleId, cb = std::move(cb)](bool success, std::int64_t,
                                                                std::exception_ptr ep)
        {
            auto self = weak.lock();
            if (!self)
            {
                // UAF 防护：facade 已析构（GameServer 关服），丢弃。
                return;
            }
            self->loop_.RunInLoop([cb = std::move(cb), success, ep, roleId]()
            {
                if (ep)
                {
                    try
                    {
                        std::rethrow_exception(ep);
                    }
                    catch (const std::exception& e)
                    {
                        LOG_WARN << "DeleteRoleArchive failed role_id=" << roleId << " what=" << e.what();
                    }
                }
                if (cb)
                {
                    cb(success);
                }
            });
        });
}

PlayerArchiveService::PlayerArchiveService(::zrpc::EventLoop& loop,
                                           mongo::PlayerMongoStorage* storage)
    : loop_(loop)
    , storage_(storage)
{
}

PlayerArchiveService::~PlayerArchiveService()
{
}
