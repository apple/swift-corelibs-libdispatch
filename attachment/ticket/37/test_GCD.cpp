#include <dispatch/dispatch.h>
#include<iostream>
#include<string>
                 
int main(int argc, char *argv[])
{
  dispatch_queue_t q;
  __block int tasks=0x10000;
            
  if(argc>1)
    tasks=atoi(argv[1]); 
              
  std::cout<<"Tasks = "<<tasks<<std::endl;
                
  q = dispatch_queue_create("test001",0);
                  
  dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT,0),
  ^{ // block 1
  // first block running in concurrent queue to create tasks.
                  
    for(int i=0;i<tasks;i++)
    {
      dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT,0),
      ^{ // block 2
      // tasks executed in concurrent queue.
        unsigned long long total=0;
                 
        for(int j=0;j<0x10000;j++)
        {  
          total+=j+((i+j)%23?(j):(i));
        }
            
        // upon task finished, queue a wrap up task to a serial queue.
        dispatch_async(q,
        ^{ // block 3
        // task run in the serial queue to count the task completion.
          --tasks;
          if(tasks<100)
            std::cout<<"@@@@@"<<tasks<<std::endl;
        
          if(0==tasks)
          {
            std::cout<<std::endl<<"*** All tasks done."<<std::endl;
          }
          else if(tasks<0)
          {
            std::cout<<std::endl<<"*** tasks is negative:"<<tasks<<std::endl;
          }
          else if(0==(tasks%100))
          {
            std::cout<<"@"<<tasks<<std::endl;
          }
                
        }); // block 3
      }); // block 2
    }
              
  }); // block 1
              
                
  std::cout<<"> "<<std::flush;
  std::string msg;
  std::cin>>msg;
                
                 
  dispatch_release(q);
                 
  return 0;
}

// EOF

