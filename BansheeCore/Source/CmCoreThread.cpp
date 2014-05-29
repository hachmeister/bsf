#include "CmCoreThread.h"
#include "BsThreadPool.h"
#include "BsTaskScheduler.h"
#include "CmFrameAlloc.h"

using namespace std::placeholders;

namespace BansheeEngine
{
	CM_THREADLOCAL CoreThread::AccessorContainer* CoreThread::mAccessor = nullptr;

	CoreThread::CoreThread()
		: mCoreThreadShutdown(false)
		, mCommandQueue(nullptr)
		, mMaxCommandNotifyId(0)
		, mSyncedCoreAccessor(nullptr)
		, mActiveFrameAlloc(0)
	{
		mFrameAllocs[0] = cm_new<FrameAlloc>();
		mFrameAllocs[1] = cm_new<FrameAlloc>();

		mCoreThreadId = CM_THREAD_CURRENT_ID;
		mCommandQueue = cm_new<CommandQueue<CommandQueueSync>>(CM_THREAD_CURRENT_ID);

		initCoreThread();
	}

	CoreThread::~CoreThread()
	{
		// TODO - What if something gets queued between the queued call to destroy_internal and this!?
		shutdownCoreThread();

		{
			CM_LOCK_MUTEX(mAccessorMutex);

			for(auto& accessor : mAccessors)
			{
				cm_delete(accessor);
			}

			mAccessors.clear();
		}

		if(mCommandQueue != nullptr)
		{
			cm_delete(mCommandQueue);
			mCommandQueue = nullptr;
		}

		cm_delete(mFrameAllocs[0]);
		cm_delete(mFrameAllocs[1]);
	}

	void CoreThread::initCoreThread()
	{
#if !CM_FORCE_SINGLETHREADED_RENDERING
#if CM_THREAD_SUPPORT
		ThreadPool::instance().run("Core", std::bind(&CoreThread::runCoreThread, this));
#else
		CM_EXCEPT(InternalErrorException, "Attempting to start a core thread but application isn't compiled with thread support.");
#endif
#endif
	}

	void CoreThread::runCoreThread()
	{
#if !CM_FORCE_SINGLETHREADED_RENDERING
		TaskScheduler::instance().removeWorker(); // One less worker because we are reserving one core for this thread

		mCoreThreadId = CM_THREAD_CURRENT_ID;
		mSyncedCoreAccessor = cm_new<CoreThreadAccessor<CommandQueueSync>>(CM_THREAD_CURRENT_ID);

		while(true)
		{
			// Wait until we get some ready commands
			Queue<QueuedCommand>* commands = nullptr;
			{
				CM_LOCK_MUTEX_NAMED(mCommandQueueMutex, lock)

				while(mCommandQueue->isEmpty())
				{
					if(mCoreThreadShutdown)
					{
						cm_delete(mSyncedCoreAccessor);
						TaskScheduler::instance().addWorker();
						return;
					}

					TaskScheduler::instance().addWorker(); // Do something else while we wait, otherwise this core will be unused
					CM_THREAD_WAIT(mCommandReadyCondition, mCommandQueueMutex, lock);
					TaskScheduler::instance().removeWorker();
				}

				commands = mCommandQueue->flush();
			}

			// Play commands
			mCommandQueue->playbackWithNotify(commands, std::bind(&CoreThread::commandCompletedNotify, this, _1)); 
		}
#endif
	}

	void CoreThread::shutdownCoreThread()
	{
#if !CM_FORCE_SINGLETHREADED_RENDERING

		{
			CM_LOCK_MUTEX(mCommandQueueMutex);
			mCoreThreadShutdown = true;
		}

		// Wake all threads. They will quit after they see the shutdown flag
		CM_THREAD_NOTIFY_ALL(mCommandReadyCondition);

		mCoreThreadId = CM_THREAD_CURRENT_ID;
#endif
	}

	CoreAccessorPtr CoreThread::getAccessor()
	{
		if(mAccessor == nullptr)
		{
			CoreAccessorPtr newAccessor = cm_shared_ptr<CoreThreadAccessor<CommandQueueNoSync>>(CM_THREAD_CURRENT_ID);
			mAccessor = cm_new<AccessorContainer>();
			mAccessor->accessor = newAccessor;

			CM_LOCK_MUTEX(mAccessorMutex);
			mAccessors.push_back(mAccessor);
		}

		return mAccessor->accessor;
	}

	SyncedCoreAccessor& CoreThread::getSyncedAccessor()
	{
		return *mSyncedCoreAccessor;
	}

	void CoreThread::submitAccessors(bool blockUntilComplete)
	{
		Vector<AccessorContainer*> accessorCopies;

		{
			CM_LOCK_MUTEX(mAccessorMutex);

			accessorCopies = mAccessors;
		}

		for(auto& accessor : accessorCopies)
			accessor->accessor->submitToCoreThread(blockUntilComplete);

		mSyncedCoreAccessor->submitToCoreThread(blockUntilComplete);
	}

	AsyncOp CoreThread::queueReturnCommand(std::function<void(AsyncOp&)> commandCallback, bool blockUntilComplete)
	{
		AsyncOp op;

		if(CM_THREAD_CURRENT_ID == getCoreThreadId())
		{
			commandCallback(op); // Execute immediately
			return op;
		}

		UINT32 commandId = -1;
		{
			CM_LOCK_MUTEX(mCommandQueueMutex);

			if(blockUntilComplete)
			{
				commandId = mMaxCommandNotifyId++;
				op = mCommandQueue->queueReturn(commandCallback, true, commandId);
			}
			else
				op = mCommandQueue->queueReturn(commandCallback);
		}

		CM_THREAD_NOTIFY_ALL(mCommandReadyCondition);

		if(blockUntilComplete)
			blockUntilCommandCompleted(commandId);

		return op;
	}

	void CoreThread::queueCommand(std::function<void()> commandCallback, bool blockUntilComplete)
	{
		if(CM_THREAD_CURRENT_ID == getCoreThreadId())
		{
			commandCallback(); // Execute immediately
			return;
		}

		UINT32 commandId = -1;
		{
			CM_LOCK_MUTEX(mCommandQueueMutex);

			if(blockUntilComplete)
			{
				commandId = mMaxCommandNotifyId++;
				mCommandQueue->queue(commandCallback, true, commandId);
			}
			else
				mCommandQueue->queue(commandCallback);
		}

		CM_THREAD_NOTIFY_ALL(mCommandReadyCondition);

		if(blockUntilComplete)
			blockUntilCommandCompleted(commandId);
	}

	void CoreThread::update()
	{
		mActiveFrameAlloc = (mActiveFrameAlloc + 1) % 2;
		mFrameAllocs[mActiveFrameAlloc]->clear();
	}

	FrameAlloc* CoreThread::getFrameAlloc() const
	{
		return mFrameAllocs[mActiveFrameAlloc];
	}

	void CoreThread::blockUntilCommandCompleted(UINT32 commandId)
	{
#if !CM_FORCE_SINGLETHREADED_RENDERING
		CM_LOCK_MUTEX_NAMED(mCommandNotifyMutex, lock);

		while(true)
		{
			// TODO - This might be causing a deadlock in Release mode. I'm thinking because mCommandsCompleted isn't marked as volatile.

			// Check if our command id is in the completed list
			auto iter = mCommandsCompleted.begin();
			for(; iter != mCommandsCompleted.end(); ++iter)
			{
				if(*iter == commandId)
					break;
			}

			if(iter != mCommandsCompleted.end())
			{
				mCommandsCompleted.erase(iter);
				break;
			}

			CM_THREAD_WAIT(mCommandCompleteCondition, mCommandNotifyMutex, lock);
		}
#endif
	}

	void CoreThread::commandCompletedNotify(UINT32 commandId)
	{
		{
			CM_LOCK_MUTEX(mCommandNotifyMutex);

			mCommandsCompleted.push_back(commandId);
		}

		CM_THREAD_NOTIFY_ALL(mCommandCompleteCondition);
	}

	CoreThread& gCoreThread()
	{
		return CoreThread::instance();
	}

	CoreThreadAccessor<CommandQueueNoSync>& gCoreAccessor()
	{
		return *CoreThread::instance().getAccessor();
	}

	void throwIfNotCoreThread()
	{
#if !CM_FORCE_SINGLETHREADED_RENDERING
		if(CM_THREAD_CURRENT_ID != CoreThread::instance().getCoreThreadId())
			CM_EXCEPT(InternalErrorException, "This method can only be accessed from the core thread.");
#endif
	}

	void throwIfCoreThread()
	{
#if !CM_FORCE_SINGLETHREADED_RENDERING
		if(CM_THREAD_CURRENT_ID == CoreThread::instance().getCoreThreadId())
			CM_EXCEPT(InternalErrorException, "This method cannot be accessed from the core thread.");
#endif
	}
}